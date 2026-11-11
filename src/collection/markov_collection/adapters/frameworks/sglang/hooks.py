"""Observe native serving calls through semantic scopes and real stream boundaries."""

from __future__ import annotations

import ctypes
import json
import os
import time
from contextlib import ExitStack, contextmanager
from functools import partial
from pathlib import Path
from typing import Any

from markov_adapters.sglang.batches import BatchTensors
from markov_adapters.sglang.pipeline import PipelineABI
from markov_adapters.sglang.state_calls import RequestStateCalls

from markov_collection.artifacts import write_json
from markov_collection.instrumentation.capture import SemanticCapture


class SGLangObservation:
    def __init__(self, description: dict[str, Any]) -> None:
        from markov_collection.adapters.frameworks.sglang.content import NativeContent
        from markov_collection.adapters.frameworks.sglang.window import ObservationWindow
        from markov_collection.adapters.runtimes.ascend.markers import AscendMarkers
        from markov_collection.adapters.runtimes.pytorch.boundaries import TensorBoundaries

        self.description = description
        self.directory = Path(description["output"])
        self.window = ObservationWindow(self.directory, description)
        self.capture = SemanticCapture(AscendMarkers())
        self.request_state = RequestStateCalls(observe=description["resource_trace"])
        self.action_values = []
        self.tensors = TensorBoundaries(self.capture)
        self.content = NativeContent(self.tensors, description)
        self.native = ctypes.CDLL(None).markov_set_runtime_enabled
        self.native.argtypes = [ctypes.c_int]
        self.native.restype = None
        self.native(0)
        self.selected = True
        self.active = False
        self.resource_history = None
        if description["resource_trace"]:
            from markov_collection.adapters.frameworks.sglang.resource_history import NativeResourceHistory

            self.resource_history = NativeResourceHistory(self)
        self.parallel = {}
        self.routing = None
        if "ep" in description["serving_input"]["configuration"]:
            from markov_collection.adapters.frameworks.sglang.native_routes import NativeExpertRoutes

            self.routing = NativeExpertRoutes(self.tensors, description)

    @contextmanager
    def call(self, role: str, original: Any, *, parameters=None, arguments=None, work=None, device=False, stream=None):
        outer = self.capture.current.get() == 0
        try:
            with ExitStack() as contexts:
                call = contexts.enter_context(self.capture.call(
                    role, original.__qualname__, f"{original.__module__}.{original.__qualname__}",
                    parameters=parameters, arguments=arguments, work=work, device=device, stream=stream,
                    constants=None if parameters is None else {
                        name: parameters[name] for name in ("page_size", "sequence_capacity") if name in parameters
                    },
                ))
                if outer:
                    contexts.enter_context(self.tensors)
                with self.request_state.record() as accesses:
                    yield call
                for key, requests in accesses.members.items():
                    identities = [self.content.aliases[request] for request in requests]
                    if not identities:
                        continue
                    field, mode = key.split("/")
                    call.record["work"][key] = identities
                    call.record.setdefault("state_accesses", []).append({
                        "space": "slot", "pool": "local", "field": field, "access": mode,
                        "members": key, "requests": identities, "interval": "device",
                    })
            if accesses.values:
                self.action_values.append({"scope": call.identity, "values": {"state": [
                    {**value, "request": self.content.aliases[value["request"]]} for value in accesses.values
                ]}})
        finally:
            if outer:
                self.tensors.retire()
        if outer:
            self.capture.runtime.retire()

    def load_model(self, original: Any, runner: Any) -> Any:
        from sglang.srt.layers.radix_attention import RadixAttention
        from sglang.srt.runtime_context import get_parallel

        from markov_adapters.sglang.groups import ParallelGroups
        from markov_collection.adapters.frameworks.sglang.module_scopes import ModelScopes

        result = original(runner)
        self.model = runner.model
        self.content.content.check_vocabulary(runner.model_config.vocab_size)
        layers = [module.layer_id for module in self.model.modules() if isinstance(module, RadixAttention)]
        topology = get_parallel()
        self.parallel = {
            "rank": self.description["rank_offset"] + runner.pp_rank * runner.tp_size + runner.tp_rank,
            "tp_rank": runner.tp_rank, "pp_rank": runner.pp_rank,
            "logical_tp": runner.tp_size, "logical_pp": runner.pp_size,
            "logical_ep": topology.moe_ep_size, "ep_rank": topology.moe_ep_rank,
            "layer_range": [min(layers), max(layers) + 1],
        }
        self.tensors.namespace = f"rank/{self.parallel['rank']}/"

        self.groups = ParallelGroups()
        self.implementation = {
            "model": runner.model_config.hf_config.to_dict(), "dtype": str(runner.model_config.dtype),
            "attention_backend": runner.server_args.attention_backend,
            "page_size": runner.server_args.page_size,
            "load_format": runner.server_args.load_format,
            "disaggregation_mode": self.description["mode"],
        }
        self.modules = ModelScopes(self, self.model)
        if self.routing is not None:
            from markov_collection.adapters.frameworks.sglang.expert_scopes import ExpertScopes

            self.routing.configure(self.modules.experts, self.modules.expert_layers, runner.model_config.hf_config.num_hidden_layers)
            self.experts = ExpertScopes(self, self.modules.experts)
        return result

    def prepare_roles(self) -> None:
        from markov_collection.adapters.frameworks.sglang.role_plan import WorkerRolePlan

        # All model stages are initialized together; a later scheduler request cannot run a world collective.
        sites = set(self.modules.bindings)
        if self.routing is not None:
            sites.update(self.experts.sites)
        facts = {
            "parallel": self.parallel,
            "groups": self.groups.document(self.description["rank_offset"]),
            "kv_layout": self.kv_layout.document(),
            "modules": [path for path, _ in self.model.named_modules() if path in sites],
            "expert_layers": self.modules.expert_layers,
        }
        self.roles = WorkerRolePlan(self.model, facts)
        if any(worker["expert_layers"] for worker in self.roles.workers) != (self.routing is not None):
            raise ValueError("MoE models require a declared expert topology and routes; dense models do not accept them")
        self.content.configure(self.roles.workers)

    def plan_roles(self) -> None:
        peers = None
        if self.description["mode"] != "null":
            from markov_adapters.sglang.kv_peers import PdPeerBindings

            workers = {}
            for path in (self.directory / "kv-pools").glob("*.json"):
                worker = json.loads(path.read_text())
                workers[worker["parallel"]["rank"]] = worker
            peers = PdPeerBindings(workers)

        # The shared warmup barrier already published every endpoint; final selection needs no native collective.
        plan = self.roles.document(peers)
        self.worker_roles = plan["workers"]
        self.selected = self.description["observe_all_workers"] or self.parallel["rank"] in plan["representatives"]
        plan["observed_roles"] = self.description["native_participants"] if self.description["observe_all_workers"] else plan["representatives"]
        if self.parallel["pp_rank"] == 0 and self.parallel["tp_rank"] == 0:
            write_json(Path(self.description["instance_directory"]) / "role-plan.json", plan, indent=2)

    def request(self, original: Any, scheduler: Any, request: Any) -> Any:
        self.window.begin(request.rid)
        if self.window.phase == "source" and self.selected and not self.active:
            self.capture.runtime.calibrate()
            self.window_scope = self.capture.runtime.scope("markov.window")
            self.window_scope.__enter__()
            self.active = True
            self.native(1)
            if self.resource_history is not None:
                self.resource_history.begin()
        return original(scheduler, request)

    def receive(self, original: Any, receiver: Any, *, ingress=False) -> Any:
        from sglang.srt.managers.io_struct import BatchTokenizedGenerateReqInput, TokenizedGenerateReqInput

        attempt = None
        if (ingress and self.resource_history is not None and self.selected
                and receiver.ps.pp_rank == 0 and receiver.ps.attn_tp_rank == 0 and receiver.ps.attn_cp_rank == 0):

            # The first source pull precedes profiler activation; warmup requests have no source identities.
            context = self.calls.scheduler_work()["context"] if self.window.phase != "warmup" else None
            attempt = {"context": context, "begin_raw_ns": time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)}

        requests = original(receiver)
        received_ns = time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)
        if ingress and receiver.ps.pp_rank != 0:
            return requests
        identities = []

        # Non-ingress ranks return None before broadcast; later PP stages receive forwarded input, not new arrivals.
        for item in requests or ():
            if isinstance(item, BatchTokenizedGenerateReqInput):
                identities.extend(request.rid for request in item)
            elif isinstance(item, TokenizedGenerateReqInput):
                identities.append(item.rid)

        if attempt is not None:
            attempt["message_count"] = len(requests or ())
        self.window.receive(identities, received_ns, ingress=ingress, attempt=attempt)
        return requests

    def batch_work(self, batch: Any) -> dict[str, Any]:
        members = BatchTensors.members(batch)
        for member in members:
            member["id"] = self.content.aliases[member["id"]]
        return {
            "phase": BatchTensors.phase(batch), "rank": self.parallel["rank"], "members": members,
            "context": {"members": self.calls.progress(batch.reqs)},
        }

    def model_work(self, batch: Any):
        work = self.batch_work(batch)
        parameters = BatchTensors.parameters(batch, sum(member["new_tokens"] for member in work["members"]))
        parameters.update(BatchTensors.model_parameters(batch, parameters["input_tokens"]))
        if self.routing is not None:
            parameters.update(self.routing.geometry.parameters(work["members"], parameters["model_tokens"]))
        return parameters, work

    def run_batch(self, original: Any, scheduler: Any, batch: Any, *args: Any, **kwargs: Any) -> Any:
        if not self.active:
            return original(scheduler, batch, *args, **kwargs)
        parameters, work = self.model_work(batch)
        with self.request_state.members(batch), self.call("run_batch", original, parameters=parameters, work=work, device=True) as call:
            if self.resource_history is not None:
                self.resource_history.batch(call.record["id"], batch)
            parent = self.capture.scopes[call.record["parent"]]
            if parent["role"] == "launch":

                # PP launch and its run_batch child share the same declared batch ABI.
                call.record["arguments"] = {name: name for name in parameters if name not in call.record["constants"]}
            result = original(scheduler, batch, *args, **kwargs)
            self.result_port(call, result, "output")
            return result

    def batch_ports(
        self, call: Any, batch: Any, direction: str,
        *, names=("request_slots", "sequence_lengths", "cache_locations"),
    ) -> None:
        columns = BatchTensors.boundaries(batch)
        for name in names:
            tensor, shape = columns[name]
            call.port(name, direction, self.tensors.observe(tensor, shape), use="device")

    def sample(self, original: Any, runner: Any, *args: Any, **kwargs: Any) -> Any:
        if not self.active:
            return original(runner, *args, **kwargs)
        with self.call("sample", original, device=True):
            return original(runner, *args, **kwargs)

    def forward(self, original: Any, runner: Any, batch: Any, *args: Any, **kwargs: Any) -> Any:
        if not self.active:
            return original(runner, batch, *args, **kwargs)

        # The initialized native registry owns these allocations across calls; a module path does not.
        with self.tensors.metadata():
            registry = runner.eager_runner._eager_registry
            owners = {}
            for name in sorted(registry.slot_names()):
                tensor = registry.get_slot(name).buffer
                if tensor is not None:
                    owner = owners.setdefault(tensor.untyped_storage()._cdata, f"eager/{name}")
                    self.tensors.bind_buffer(tensor, owner)
        with self.call("model", original, device=True) as call:
            call.record["parameters"].update(input_tokens=int(batch.input_ids.numel()), members=int(batch.batch_size))
            self.batch_ports(call, batch, "input")
            return original(runner, batch, *args, **kwargs)

    def model_geometry(self, tokens: int, members: int) -> None:

        # Check actual model tensors against the shared native padding contract, without replacing predicted features.
        parameters = self.capture.scopes[self.capture.current.get()]["parameters"]
        if (tokens, members) != (parameters["model_tokens"], parameters["model_members"]):
            raise ValueError("native model geometry differs from the declared eager batch layout")

    def result_port(self, call: Any, output: Any, direction: str) -> None:
        tensor = output.next_token_ids
        if tensor is not None:

            # Result processing materializes tokens on the CPU; readiness gates the completed read.
            use = "host" if direction == "input" or tensor.device.type == "cpu" else "device"
            call.port(
                "next_token_ids", direction, self.tensors.observe(tensor, ["members"]), use=use, at_return=direction == "input",
            )

        proxy = output.pp_hidden_states_proxy_tensors
        if direction == "output" and proxy is not None:
            for name, tensor in PipelineABI.proxy_tensors(proxy).items():
                rule = self.tensors.shape_rule(tensor)
                call.port(f"proxy.{name}", direction, self.tensors.observe(tensor, rule), use="device")

    def can_flush(self) -> bool:
        if not self.scheduler.is_fully_idle() or PipelineABI.pending_model_sends(self.scheduler):
            return False

        # Input dispatch may be mid-pipeline round; only on_idle closes the observed control flow.
        return self.window.phase is None or self.window.drained

    def flush_wrapper(self, original: Any, wrapper: Any, *, flush_cache: Any, is_fully_idle: Any, ipc_channels: Any) -> None:
        self.scheduler = flush_cache.__self__
        original(wrapper, flush_cache=flush_cache, is_fully_idle=self.can_flush, ipc_channels=ipc_channels)

    def drain(self, scheduler: Any) -> None:
        if PipelineABI.pending_model_sends(scheduler):
            return
        if not scheduler.is_fully_idle() or self.capture.current.get() != 0:
            return
        self.window.group_idle(self.parallel["rank"])
        if not self.window.inputs_complete() or self.window.drained:
            return
        import torch

        # Drain the observed device tails before native flush changes logical KV state.
        torch.npu.synchronize()
        self.native(0)
        if self.active:
            self.window_scope.__exit__(None, None, None)
            self.capture.runtime.calibrate()
            self.active = False
            if self.resource_history is not None:
                self.resource_history.close()
            if self.description["pp_size"] > 1:
                self.pipeline.signals.enabled = False
        self.window.drain(self.parallel["rank"])

    def flush(self, original: Any, scheduler: Any, empty_cache: bool = True) -> Any:
        result = original(scheduler, empty_cache=False)
        if result and self.window.drained:
            registration = self.kv_transfers.registration()
            if registration is not None:

                # Every endpoint supplies layout evidence even when its computation is represented by another worker.
                write_json(self.directory / "kv-pools" / f"{self.parallel['rank']}.json", {
                    "mode": self.description["mode"], "parallel": self.parallel, "layout": self.kv_layout.document(),
                    "page_size": self.implementation["page_size"], "registration": registration,
                })
            if self.window.phase == "source" and self.selected:
                import torch
                import torch_npu

                self.active = False
                self.implementation["scheduling"] = {
                    "chunked_prefill": scheduler.chunked_prefill_size is not None,
                    "mixed_batches": scheduler.is_mixed_chunk,
                }
                recording = self.capture.document({
                    "framework": "sglang", "torch": torch.__version__, "torch_npu": torch_npu.__version__,
                    "device": torch.npu.get_device_name(), "device_index": torch.npu.current_device(),
                    "parallel": self.parallel, "implementation": self.implementation, "kv_layout": self.kv_layout.document(),
                    "worker_roles": self.worker_roles,
                    "initial_state": {"logical_kv": "empty_after_native_flush", "workspace": "uniform_native_warmup"},
                    "closure": {"native_idle": True, "device_drained": True, "pending_native_transfers": 0},
                })
                role = self.worker_roles[str(self.parallel["rank"])]
                for name in ("expert_layers", "kv_peers"):
                    if name in role:
                        recording["environment"][name] = role[name]
                recording["request_receipts"] = self.window.receipts
                recording["device_clock"] = self.capture.runtime.clock_samples
                recording["host_observation"] = self.capture.runtime.host_observation()
                recording["request_receive_rounds"] = self.window.receive_rounds
                recording["request_ingress"] = self.window.ingress
                recording["state_queries"] = self.calls.queries
                recording["protocol_states"] = self.kv_transfers.states
                recording["kv_session"] = None if registration is None else registration["session"]
                recording["kv_endpoint"] = None if registration is None else registration["control_endpoint"]
                if self.resource_history is not None:
                    recording["request_input_attempts"] = self.window.input_attempts
                    recording["action_values"] = self.action_values
                    write_json(self.directory / "resources" / f"rank-{self.parallel['rank']}.json", self.resource_history.document())
                write_json(self.directory / f"rank-{self.parallel['rank']}.json", recording)
            self.window.prepared_phase = self.window.phase
        return result

    def idle(self, original: Any, scheduler: Any) -> Any:
        if scheduler.is_fully_idle():
            self.drain(scheduler)
            if self.window.prepared_phase is not None:
                self.window.complete(self.parallel["rank"], scheduler.idle_sleeper is not None)
        return original(scheduler)

    def install(self) -> None:
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        from markov_collection.adapters.frameworks.sglang.communications import NativeCommunications
        from markov_collection.adapters.frameworks.sglang.profiles import NativeProfiles
        from markov_collection.adapters.frameworks.sglang.kv_scopes import NativeKvScopes
        from markov_collection.adapters.frameworks.sglang.flow_scopes import FlowScopes
        from markov_adapters.sglang.metadata_transfers import MetadataTransfers

        targets = {
            "sglang.srt.model_executor.model_runner.ModelRunner.load_model": self.load_model,
            "sglang.srt.model_executor.model_runner.ModelRunner.forward": self.forward,
            "sglang.srt.managers.scheduler.Scheduler.handle_generate_request": self.request,
            "sglang.srt.managers.scheduler_components.request_receiver.SchedulerRequestReceiver.recv_requests": self.receive,
            "sglang.srt.managers.scheduler_components.request_receiver.SchedulerRequestReceiver._pull_raw_reqs": partial(self.receive, ingress=True),
            "sglang.srt.managers.scheduler.Scheduler.run_batch": self.run_batch,
            "sglang.srt.model_executor.model_runner.ModelRunner.sample": self.sample,
            "sglang.srt.managers.scheduler.Scheduler.flush_cache": self.flush,
            "sglang.srt.managers.scheduler.Scheduler.on_idle": self.idle,
            "sglang.srt.managers.scheduler_components.flush_wrapper.SchedulerFlushWrapper.__init__": self.flush_wrapper,
        }
        for target, hook in targets.items():
            HookRegistry.register(target, hook, HookType.AROUND)
        self.request_state.install()
        NativeProfiles(self).install()
        NativeCommunications(self).install()
        self.kv_transfers = NativeKvScopes(self)
        self.kv_transfers.install()
        self.calls = FlowScopes(self)
        self.calls.install()
        if self.description["pp_size"] > 1:
            from markov_collection.adapters.frameworks.sglang.pipeline_scopes import PipelineScopes

            self.pipeline = PipelineScopes(self)
            self.pipeline.install()
        self.content.install()
        if self.routing is not None:
            self.routing.install()

        # The caller fragment must close before the runtime measures or blocks on CPU metadata.
        MetadataTransfers(self.calls).install()


def install() -> None:
    path = os.environ.get("MARKOV_GRAPH_SESSION")
    if path is not None:
        SGLangObservation(json.loads(Path(path).read_text())).install()
