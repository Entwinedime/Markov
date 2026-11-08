"""Run the native scheduler loop against core resource and execution services."""

from dataclasses import replace
from functools import partial
from pathlib import Path

import torch
from markov_adapters.sglang.batches import BatchTensors
from markov_adapters.sglang.experts import ExpertBatch
from markov_adapters.sglang.pipeline import PipelineABI, PipelineMessages
from markov_adapters.sglang.state_calls import RequestStateCalls
from markov_control.adapters.frameworks.sglang.batches import BatchNames
from markov_control.adapters.frameworks.sglang.prefix_cache import PrefixCache
from markov_control.adapters.frameworks.sglang.requests import NativeRequestReceiver, RequestInput
from markov_control.adapters.frameworks.sglang.flow import FlowExecution
from markov_control.adapters.frameworks.sglang.resources import AllocationCalls
from markov_control.adapters.frameworks.sglang.runner import MetadataRunner
from markov_control.adapters.frameworks.sglang.signals import PipelineSignals
from markov_control.adapters.frameworks.sglang.workers import NativeStopped
from markov_control.adapters.runtimes.tensors import TensorValues
from markov_workload.expert_routes import ExpertRoutes
from sglang.srt.entrypoints.engine import _compute_parallelism_ranks
from sglang.srt.layers.logits_processor import LogitsProcessorOutput
from sglang.srt.managers import schedule_batch
from sglang.srt.managers import scheduler as scheduler_module
from sglang.srt.managers.scheduler import Scheduler
from sglang.srt.managers.utils import GenerationBatchResult
from sglang.srt.mem_cache.registry import register_radix_cache_backend
from sglang.srt.model_executor import model_runner
from sglang.srt.model_executor.forward_batch_info import PPProxyTensors
from sglang.srt.server_args import PortArgs, ServerArgs


class NativeScheduler(Scheduler):
    def __init__(self, peer, settings, model_path: str, source, rank: int, workload, workers, local) -> None:
        self.peer = peer
        self.instance_id = source["id"]
        worker = source["workers"][rank]
        self.graph_rank = worker["rank"]
        self.group = worker["group"]
        self.waiting_for_change = False
        self.launching = False
        self.pd = None
        if source["mode"] != "null":
            from markov_control.adapters.frameworks.sglang.pd import NativePdBackend

            self.pd = NativePdBackend(self, workers, local)
            self.pd.install_resources()
        self.signals = PipelineSignals(self)
        self.values = TensorValues(f"{source['id']}/{self.graph_rank}")
        self.batch_names = BatchNames()
        self.request_state = RequestStateCalls(observe=settings["observe"])
        model_runner.ModelRunner = partial(
            MetadataRunner, peer=peer, rendezvous=Path(model_path, "group").as_uri(), kv_layout=worker["kv_layout"],
        )
        cache = source["cache"]
        register_radix_cache_backend("markov", partial(PrefixCache.build, expected=cache))
        scheduler_module.create_scheduler_watchdog = self.workflow_watchdog
        args = ServerArgs(
            model_path=model_path,
            dtype=source["dtype"].removeprefix("torch."),
            tp_size=source["tp"],
            ep_size=source["ep"],
            pp_size=source["pp"],
            moe_a2a_backend=source["moe_a2a_backend"],
            deepep_mode=source["deepep_mode"],
            pp_max_micro_batch_size=settings["microbatch_requests"],
            device="cpu",
            load_format="dummy",
            skip_tokenizer_init=True,
            max_total_tokens=settings["max_total_tokens"],
            max_running_requests=settings["max_running_requests"],
            max_prefill_tokens=settings["max_prefill_tokens"],
            page_size=settings["page_size"],
            attention_backend="torch_native",
            disable_radix_cache=cache["disabled"],
            radix_cache_backend="markov",
            radix_eviction_policy=cache["eviction_policy"],
            chunked_prefill_size=settings["prefill_chunk_tokens"] or -1,
            enable_mixed_chunk=settings["mixed_batches"],
            disable_overlap_schedule=True,
            disable_cuda_graph=True,
            disable_custom_all_reduce=True,
            disaggregation_mode=source["mode"],
            disaggregation_decode_enable_radix_cache=source["mode"] == "decode" and not cache["disabled"],
            random_seed=13,
        )
        if (args.tp_size, args.pp_size, args.ep_size) != (source["tp"], source["pp"], source["ep"]):
            raise ValueError("native configuration changed the supplied parallel topology")
        cp_rank, moe_dp_rank, ep_rank = _compute_parallelism_ranks(args, rank % source["tp"])
        if ep_rank != worker["ep_rank"]:
            raise ValueError("template expert rank differs from native parallel initialization")
        self.experts = None
        has_experts = any(worker["expert_layers"] for worker in source["workers"])
        if has_experts != (settings["expert_routes"] is not None):
            raise ValueError("native model and supplied expert route conditions disagree")
        if has_experts:
            routes = ExpertRoutes(
                settings["expert_routes"], workload["requests"], worker["expert_layers"],
                source["model_config"]["num_hidden_layers"], source["ep"] - 1,
            )
            self.experts = ExpertBatch(routes, source["ep"], ep_rank, args.moe_a2a_backend, args.deepep_mode)
            if source["expert_layout"] != self.experts.layout:
                raise ValueError("native control and source expert layout differ")

        super().__init__(
            args,
            PortArgs.init_new(args),
            gpu_id=0,
            tp_rank=rank % source["tp"],
            moe_ep_rank=ep_rank,
            pp_rank=rank // source["tp"],
            attn_cp_rank=cp_rank,
            moe_dp_rank=moe_dp_rank,
            dp_rank=None,
        )
        if settings["mixed_batches"] != self.is_mixed_chunk:
            raise ValueError("native configuration did not retain the requested mixed-batch mode")

        # Preserve native receive, dispatch, selection, result processing and idle housekeeping.
        self.request_input = RequestInput(
            peer,
            self.model_config.vocab_size,
            workload,
            settings["expert_routes"] if not cache["disabled"] else None,
            source["mode"],
            {layer["layer"] for worker in source["workers"] for layer in worker["expert_layers"]},
        )
        self.request_receiver = NativeRequestReceiver(
            replace(self.request_receiver, recv_from_tokenizer=self.request_input), self
        )
        self.model_worker.forward_batch_generation = self.model_result
        self.request_state.install()
        self.calls = FlowExecution(self)
        AllocationCalls(schedule_batch)
        if self.pd is not None:
            self.pd.install_calls()

        if source["pp"] > 1:
            self.pipeline_messages = PipelineMessages(self.calls)
            self.signals.install()

    def description(self, batch):
        if batch is None:
            return {}, self.batch_names.describe(None)
        members = BatchTensors.members(batch)
        for member in members:
            member["id"] = self.request_input.requests[int(member["id"])]["id"]
        work = {**self.batch_names.describe(batch), "phase": BatchTensors.phase(batch), "rank": self.graph_rank, "members": members}
        work["context"] = {"members": self.calls.progress(batch.reqs)}
        return BatchTensors.parameters(batch, sum(member["new_tokens"] for member in members)), work

    def action(self, name, parameters, work, outputs=(), *, condition=None, tensor_inputs=None, tensor_outputs=None, allocations=None):
        condition = {"phase": work.get("phase", "idle"), "state_accesses": RequestStateCalls.condition(work), **(condition or {})}

        # Child calls can create batches before their parent resumes; liveness belongs to submission time.
        work["live_batches"] = self.batch_names.live()
        work["live_signals"] = list(self.signals.live)
        if self.pd is not None:
            work["live_signals"].extend(sorted(self.pd.live_signals()))
        work["allocations"] = {name: self.values.allocate(tensor) for name, tensor in (allocations or {}).items()}
        work["values"] = self.values.inputs(tensor_inputs or {})
        work["live_values"] = self.values.live()

        # Native decisions and resource calls have run; execution must complete this return before native continues.
        returned = self.peer.call("action", action=name, condition=condition, parameters=parameters, work=work, outputs=list(outputs))
        self.values.publish(tensor_outputs or {}, returned["values"])
        return returned

    def model_work(self, batch):
        parameters, work = self.description(batch)
        parameters.update(BatchTensors.model_parameters(batch, parameters["input_tokens"]))
        if self.experts is not None:
            parameters.update(self.experts.parameters(work["members"], parameters["model_tokens"]))
        return parameters, work

    def run_batch(self, batch, *args, **kwargs):
        if self.launching:
            return super().run_batch(batch, *args, **kwargs)
        parameters, work = self.model_work(batch)
        with self.request_state.members(batch), self.request_state.record() as accesses:
            result = super().run_batch(batch, *args, **kwargs)
        self.bind_state(work, accesses)
        self.action("run_batch", parameters, work)
        return result

    def _pp_launch_batch(self, mb_id, pp_proxy_tensors, mb_metadata, last_rank_comm_queue):
        parameters, work = self.model_work(self.cur_batch)

        # The native launch owns run_batch and its recorded completion; submit the parent graph once.
        self.launching = True
        try:
            with self.signals.producing(), self.request_state.members(self.cur_batch), self.request_state.record() as accesses:
                result, event = super()._pp_launch_batch(mb_id, pp_proxy_tensors, mb_metadata, last_rank_comm_queue)
        finally:
            self.launching = False
        work["signals"] = {"completion": event._markov_signal.identity}
        self.bind_state(work, accesses)
        inputs = {} if pp_proxy_tensors is None else {f"proxy.{name}": tensor for name, tensor in PipelineABI.proxy_tensors(pp_proxy_tensors).items()}
        if result.pp_hidden_states_proxy_tensors is None:
            outputs = {"next_token_ids": result.next_token_ids}
        else:
            outputs = {f"proxy.{name}": tensor for name, tensor in PipelineABI.proxy_tensors(result.pp_hidden_states_proxy_tensors).items()}
        self.action("launch", parameters, work, tensor_inputs=inputs, tensor_outputs=outputs)
        return result, event

    def _pp_prep_batch_result(self, batch, mb_metadata, pp_outputs):
        parameters, work = self.description(batch)
        received = pp_outputs["next_token_ids"]
        tokens = self.next_tokens(batch)

        # Workload token values drive native bookkeeping; the received core value retains its data dependency.
        self.values.alias(received, tokens)
        pp_outputs["next_token_ids"] = tokens
        with self.request_state.members(batch), self.request_state.record() as accesses:
            result = super()._pp_prep_batch_result(batch, mb_metadata, pp_outputs)
        self.bind_state(work, accesses)
        self.action(
            "output_prepare", parameters, work, tensor_inputs={"next_token_ids": tokens},
            tensor_outputs={"next_token_ids": result.next_token_ids},
        )
        return result

    def _pp_make_skip_output_result(self, batch, mb_metadata):
        parameters, work = self.description(batch)
        with self.signals.producing():
            outputs, result, event = super()._pp_make_skip_output_result(batch, mb_metadata)
        work["signals"] = {"completion": event._markov_signal.identity}
        self.action("output_skip", parameters, work, tensor_outputs={"next_token_ids": result.next_token_ids})
        return outputs, result, event

    def next_tokens(self, batch):
        return torch.tensor([self.request_input.output_token(req.rid) for req in batch.reqs], dtype=torch.int64)

    def bind_state(self, work, accesses):
        work.update({
            name: [self.request_input.requests[int(identity)]["id"] for identity in requests]
            for name, requests in accesses.members.items()
        })

    def model_result(self, batch, **kwargs):
        if not self.pp_group.is_last_rank:
            parameters, work = self.model_work(batch)
            condition = {"phase": work["phase"], "state_accesses": RequestStateCalls.condition(self.request_state.accesses.get().members)}
            outputs = self.peer.call("describe", action="launch", condition=condition, parameters=parameters)
            tensors = {
                name.removeprefix("proxy."): torch.empty(
                    layout["shape"], dtype=getattr(torch, layout["dtype"].removeprefix("torch.")), device="meta",
                )
                for name, layout in outputs.items() if name.startswith("proxy.")
            }
            if not tensors:
                raise ValueError("pipeline launch template has no intermediate tensor outputs")

            # Intermediate names and geometry come from the captured template, not model-family guesses.
            return GenerationBatchResult(pp_hidden_states_proxy_tensors=PPProxyTensors(tensors))

        # These token values drive native request bookkeeping; their device production belongs to the action graph.
        return GenerationBatchResult(
            logits_output=LogitsProcessorOutput(next_token_logits=None),
            next_token_ids=self.next_tokens(batch),
        )

    @staticmethod
    def workflow_watchdog(*args, **kwargs):

        # Workflow owns wall-clock deadlines; native watchdog diagnostics would race the synchronous resource channel.
        return None

    def process_batch_result(self, batch, result):
        self.calls.process(super().process_batch_result, batch, result)

    def on_idle(self):
        self.waiting_for_change = self.pd is not None and self.pd.waiting()
        idle = self.is_fully_idle()
        if idle:
            self.drain_transfers()
        super().on_idle()
        if not idle:
            return
        message = {"kind": "idle", "waiting": self.ps.pp_rank == 0 and self.ps.tp_rank == 0}

        # Only ingress sleeps; its peers can still reach the native control boundary.
        if message["waiting"]:
            self.peer.call("idle", waiting=True)
        else:
            self.peer.send(message)

    def drain_transfers(self):
        if self.ps.pp_size > 1:

            # Native logical idle can still own asynchronous sends from the preceding microbatch round.
            for work in (self.send_req_work, self.send_proxy_work, self.send_output_work):
                self._pp_commit_comm_work(work)

    def stop(self, requests):
        self.drain_transfers()
        if self.ps.pp_size > 1 and not self.pp_group.is_last_rank:

            # Forward shutdown through the same native request path before leaving downstream receivers behind.
            self._pp_send_pyobj_to_next_stage(requests, async_send=False)
        raise NativeStopped()
