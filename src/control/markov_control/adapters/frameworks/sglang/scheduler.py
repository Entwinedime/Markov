"""Run the native scheduler loop against core resource and execution services."""

from dataclasses import replace
from functools import partial
from pathlib import Path

import torch
from markov_control.adapters.frameworks.sglang.batches import BatchAllocation, NativeBatch
from markov_control.adapters.frameworks.sglang.requests import RequestInput
from markov_control.adapters.frameworks.sglang.runner import MetadataRunner
from markov_control.peer import ControlPeer
from sglang.srt.layers.logits_processor import LogitsProcessorOutput
from sglang.srt.managers import schedule_batch
from sglang.srt.managers import scheduler as scheduler_module
from sglang.srt.managers.scheduler import Scheduler
from sglang.srt.managers.utils import GenerationBatchResult
from sglang.srt.model_executor import model_runner
from sglang.srt.server_args import PortArgs, ServerArgs


class PublishedRequests:
    def __init__(self) -> None:
        self.identities = set()

    def send_output(self, output, *args, **kwargs):
        self.identities.update(output.rids)


class ResultReady:
    def __init__(self, peer: ControlPeer) -> None:
        self.peer = peer

    def synchronize(self):

        # The native consumer waits for the observed result-read subgraph, not every device stream.
        self.peer.call("read_result")


class NativeScheduler(Scheduler):
    def __init__(self, peer, settings, model_path: str, source, rank: int) -> None:
        self.peer = peer
        model_runner.ModelRunner = partial(MetadataRunner, peer=peer, rendezvous=Path(model_path, "group").as_uri())
        scheduler_module.create_scheduler_watchdog = self.workflow_watchdog
        BatchAllocation(schedule_batch)
        schedule_batch.ScheduleBatch = NativeBatch
        scheduler_module.ScheduleBatch = NativeBatch
        args = ServerArgs(
            model_path=model_path,
            dtype=source["dtype"].removeprefix("torch."),
            tp_size=source["tp"],
            ep_size=source["ep"],
            device="cpu",
            load_format="dummy",
            skip_tokenizer_init=True,
            max_total_tokens=settings["max_total_tokens"],
            max_running_requests=settings["max_running_requests"],
            max_prefill_tokens=settings["max_prefill_tokens"],
            page_size=settings["page_size"],
            attention_backend="torch_native",
            disable_radix_cache=True,
            chunked_prefill_size=-1,
            disable_overlap_schedule=True,
            disable_cuda_graph=True,
            disable_custom_all_reduce=True,
            random_seed=13,
        )
        super().__init__(
            args,
            PortArgs.init_new(args),
            gpu_id=0,
            tp_rank=rank,
            moe_ep_rank=rank % source["ep"],
            pp_rank=0,
            attn_cp_rank=0,
            moe_dp_rank=0,
            dp_rank=None,
        )

        # Preserve native receive, dispatch, selection, result processing and idle housekeeping.
        self.request_receiver = replace(
            self.request_receiver, recv_from_tokenizer=RequestInput(peer, self.model_config.vocab_size)
        )
        self.model_worker.forward_batch_generation = self.model_result
        self.published = PublishedRequests()
        self.output_streamer.send_to_detokenizer = self.published

    def update_running_batch(self, batch):
        batch.control_selection = True
        try:
            return super().update_running_batch(batch)
        finally:
            batch.control_selection = False

    def model_result(self, batch, **kwargs):
        self.peer.call("model")

        # Workloads specify output lengths, not generated text; these token values carry only control progress.
        return GenerationBatchResult(
            logits_output=LogitsProcessorOutput(next_token_logits=None),
            next_token_ids=torch.ones(len(batch.reqs), dtype=torch.int64),
            copy_done=ResultReady(self.peer),
        )

    @staticmethod
    def workflow_watchdog(*args, **kwargs):

        # Workflow owns wall-clock deadlines; native watchdog diagnostics would race the synchronous resource channel.
        return None

    def process_batch_result(self, batch, result):
        self.published.identities.clear()
        super().process_batch_result(batch, result)
        completion = self.peer.call(
            "result",
            members=[
                {
                    "request": int(req.rid),
                    "generated_tokens": len(req.output_ids),
                    "finished": req.finished(),
                    "emits_output": req.rid in self.published.identities,
                }
                for req in batch.reqs
            ],
        )
        self.gracefully_exit = completion["done"]

    def on_idle(self):
        super().on_idle()
        self.peer.send({"kind": "idle"})
