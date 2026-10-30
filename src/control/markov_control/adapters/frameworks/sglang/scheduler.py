"""Run the native scheduler loop against core resource and execution services."""

from dataclasses import replace
from functools import partial
from pathlib import Path

import torch
from markov_control.adapters.frameworks.sglang.batches import BatchAllocation, NativeBatch
from markov_control.adapters.frameworks.sglang.prefix_cache import PrefixCache
from markov_control.adapters.frameworks.sglang.requests import NativeRequestReceiver, RequestInput
from markov_control.adapters.frameworks.sglang.resources import ChunkIndices
from markov_control.adapters.frameworks.sglang.runner import MetadataRunner
from markov_control.peer import ControlPeer
from sglang.srt.layers.logits_processor import LogitsProcessorOutput
from sglang.srt.managers import schedule_batch
from sglang.srt.managers import scheduler as scheduler_module
from sglang.srt.managers.scheduler import Scheduler
from sglang.srt.managers.utils import GenerationBatchResult
from sglang.srt.mem_cache.registry import register_radix_cache_backend
from sglang.srt.model_executor import model_runner
from sglang.srt.server_args import PortArgs, ServerArgs


class PublishedRequests:
    def __init__(self, enabled: bool, allocator) -> None:
        self.identities = set()
        self.enabled = enabled
        self.allocator = allocator

    def send_output(self, output, *args, **kwargs):
        if self.enabled:
            self.identities.update(output.rids)

    def complete(self, peer, batch, kind):
        tail = {}
        trace = self.allocator.trace
        if trace is not None and trace.result_tail is not None:
            tail["index_tail"], trace.result_tail = trace.result_tail, None
        return peer.call(
            kind,
            members=[
                {
                    "request": int(req.rid),
                    "generated_tokens": len(req.output_ids),
                    "finished": req.finished(),
                    "emits_output": req.rid in self.identities,
                }
                for req in batch.reqs
            ],
            **tail,
        )


class ResultReady:
    def __init__(self, peer: ControlPeer, work) -> None:
        self.peer = peer
        self.work = work

    def synchronize(self):

        # The native consumer waits for the observed result-read subgraph, not every device stream.
        self.peer.call("read_result", **self.work)


class NativeScheduler(Scheduler):
    def __init__(self, peer, settings, model_path: str, source, rank: int, workload) -> None:
        self.peer = peer
        self.control_rank = rank
        self.graph_rank = source["ranks"][rank]
        self.control_round = 0
        model_runner.ModelRunner = partial(MetadataRunner, peer=peer, rendezvous=Path(model_path, "group").as_uri())
        register_radix_cache_backend("markov", PrefixCache.build)
        scheduler_module.create_scheduler_watchdog = self.workflow_watchdog
        BatchAllocation(schedule_batch)
        schedule_batch.ScheduleBatch = NativeBatch
        scheduler_module.ScheduleBatch = NativeBatch
        args = ServerArgs(
            model_path=model_path,
            dtype=source["dtype"].removeprefix("torch."),
            tp_size=source["tp"],
            ep_size=source["ep"],
            pp_size=source["pp"],
            pp_max_micro_batch_size=settings["microbatch_requests"],
            device="cpu",
            load_format="dummy",
            skip_tokenizer_init=True,
            max_total_tokens=settings["max_total_tokens"],
            max_running_requests=settings["max_running_requests"],
            max_prefill_tokens=settings["max_prefill_tokens"],
            page_size=settings["page_size"],
            attention_backend="torch_native",
            disable_radix_cache=source["disable_radix_cache"],
            radix_cache_backend="markov",
            chunked_prefill_size=settings["prefill_chunk_tokens"] or -1,
            enable_mixed_chunk=settings["mixed_batches"],
            disable_overlap_schedule=True,
            disable_cuda_graph=True,
            disable_custom_all_reduce=True,
            disaggregation_mode=source["mode"],
            random_seed=13,
        )
        super().__init__(
            args,
            PortArgs.init_new(args),
            gpu_id=0,
            tp_rank=rank % source["tp"],
            moe_ep_rank=rank % source["ep"],
            pp_rank=rank // source["tp"],
            attn_cp_rank=0,
            moe_dp_rank=0,
            dp_rank=None,
        )
        if settings["mixed_batches"] != self.is_mixed_chunk:
            raise ValueError("native configuration did not retain the requested mixed-batch mode")
        if source["disable_radix_cache"] != self.server_args.disable_radix_cache:
            raise ValueError("native configuration did not retain the source prefix-cache mode")

        # Preserve native receive, dispatch, selection, result processing and idle housekeeping.
        bootstrap = ("markov-prefill", args.disaggregation_bootstrap_port) if source["mode"] != "null" else None
        self.request_input = RequestInput(
            peer,
            self.model_config.vocab_size,
            bootstrap,
            workload,
            settings["expert_routes"] if not source["disable_radix_cache"] else None,
        )
        self.request_receiver = NativeRequestReceiver(
            replace(self.request_receiver, recv_from_tokenizer=self.request_input)
        )
        self.model_worker.forward_batch_generation = self.model_result
        ChunkIndices(self.tree_cache, peer)
        self.published = PublishedRequests(self.ps.pp_rank == 0, self.token_to_kv_pool_allocator)
        self.output_streamer.send_to_detokenizer = self.published

    def current_lane(self):
        return 0

    def begin_iteration(self):
        self.peer.context = {"flow": self.control_round, "lane": self.current_lane()}
        self.control_round += 1
        self.peer.send({"kind": "begin"})

    def get_next_batch_to_run(self):
        self.begin_iteration()
        return self.prepare(super().get_next_batch_to_run())

    def prepare(self, batch):
        if batch is not None:
            batch.control_work = dict(self.peer.context)
            prefill = batch.forward_mode.is_extend()
            lengths = batch.extend_lens if prefill else [1] * len(batch.reqs)
            decoding = {req.rid for req in batch.decoding_reqs or ()}

            # Final batch columns describe model input; request objects can retain older prefix-cache views.
            self.peer.call(
                "prepare",
                phase="mixed" if batch.forward_mode.is_mixed() else "prefill" if prefill else "decode",
                members=[
                    {
                        "request": int(req.rid),
                        "generated_tokens": len(req.output_ids),
                        "context_tokens": sequence - tokens,
                        "new_tokens": tokens,
                        "phase": "decode" if not prefill or req.rid in decoding else "prefill",
                    }
                    for req, sequence, tokens in zip(batch.reqs, batch.seq_lens_cpu.tolist(), lengths, strict=True)
                ],
            )
        return batch

    def update_running_batch(self, batch):
        batch.control_selection = True
        try:
            return super().update_running_batch(batch)
        finally:
            batch.control_selection = False

    def model_result(self, batch, **kwargs):
        self.peer.call("model")

        # Generated values obey the same private continuation as native observations.
        return GenerationBatchResult(
            logits_output=LogitsProcessorOutput(next_token_logits=None),
            next_token_ids=torch.tensor(
                [self.request_input.output_token(req.rid) for req in batch.reqs], dtype=torch.int64
            ),
            copy_done=ResultReady(self.peer, dict(self.peer.context)),
        )

    @staticmethod
    def workflow_watchdog(*args, **kwargs):

        # Workflow owns wall-clock deadlines; native watchdog diagnostics would race the synchronous resource channel.
        return None

    def process_batch_result(self, batch, result):
        previous = self.peer.context
        self.peer.context = batch.control_work
        try:
            self.published.identities.clear()
            super().process_batch_result(batch, result)
            completion = self.published.complete(self.peer, batch, "result")
            self.gracefully_exit = completion["done"]
            if self.ps.pp_size == 1:
                self.peer.call("release_work")
        finally:
            self.peer.context = previous

    def on_idle(self):
        super().on_idle()
        self.peer.send({"kind": "idle"})
