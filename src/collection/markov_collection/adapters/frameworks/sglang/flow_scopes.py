"""Capture native lifecycle calls interleaved with their caller segments."""

from contextlib import contextmanager

from markov_adapters.sglang.allocation import AllocationABI
from markov_adapters.sglang.batches import BatchTensors
from markov_adapters.sglang.calls import CallSegments
from markov_adapters.sglang.clocks import DecisionClocks
from markov_adapters.sglang.disaggregated_calls import DisaggregatedCalls
from markov_adapters.sglang.host_reads import HostReads
from markov_adapters.sglang.kv_layout import MhaKvLayout
from markov_adapters.sglang.metadata_collectives import MetadataCollectives
from markov_adapters.sglang.resources import ResourceCalls
from markov_adapters.sglang.selection import BatchSelection


class FlowScopes(CallSegments):
    def __init__(self, observation) -> None:
        super().__init__()
        self.observation = observation
        self.request_state = observation.request_state
        self.resources = ResourceCalls(self)
        self.selection = BatchSelection(self)
        self.queries = []

    @property
    def enabled(self):
        return self.observation.active and self.observation.selected

    @property
    def protocol_enabled(self):

        # Protocol facts cover every worker, independently of representative device profiling.
        window = self.observation.window
        return window.phase == "source" and not window.drained

    @property
    def scheduler(self):
        return self.observation.scheduler

    @property
    def groups(self):
        return self.observation.groups

    def request_id(self, native_id):
        return self.observation.content.aliases[native_id]

    def work(self, batch, members=None, *, phase="prefill"):
        work = {} if batch is None else {"context": {"members": self.progress(batch.reqs)}}
        if members is not None:
            work.update(
                phase=phase, rank=self.observation.parallel["rank"],
                members=[{**member, "id": self.request_id(member["id"])} for member in members],
            )
        return work

    def completed(self, requests):
        return [
            {"id": self.observation.content.aliases[req.rid], "generated_tokens": len(req.output_ids), "finished": req.finished()}
            for req in requests
        ]

    @contextmanager
    def read_boundary(self, kind, original, work, condition):
        owner = self.observation
        with owner.call("query", original, parameters={}, work=work) as call:
            call.record["bindings"].update(action=True, condition=condition)
            query = {"kind": kind, "scope": call.identity}
            if "read" in condition:
                query["read"] = condition["read"]
            with owner.capture.runtime.service_scope(query):
                yield query
            self.queries.append(query)

    def select(self, original, scheduler, *args, **kwargs):
        return self.selection.select(original, scheduler, *args, **kwargs)

    def collective(self, original, boundary, arguments):
        owner = self.observation
        collective = boundary.condition["collective"]
        role = collective["group"]
        descriptor = owner.worker_roles[str(owner.parallel["rank"])]["groups"][role]
        members = descriptor["members"]
        root = collective["root"]
        values, outputs = MetadataCollectives.snapshot(arguments, root) if owner.resource_history is not None else (None, None)
        with owner.call(boundary.operation, original, parameters=boundary.parameters, work=boundary.work) as call:
            call.record["bindings"].update(action=True, condition=boundary.condition, collective_group=role)
            call.record["communication"] = {
                "channel": descriptor["channel"], "operation": collective["operation"], "members": members,
                "required": members if root is None else [members[root]], "rank": owner.parallel["rank"], "order": 0,
            }
            result = original(**arguments)

        # Values are optional evidence outside measured work; they never become template parameters.
        if values is not None:
            values["output"] = outputs.flatten().tolist()
            owner.action_values.append({"scope": call.identity, "values": {"collective": values}})
        return result

    @contextmanager
    def boundary(self, original, boundary):
        owner = self.observation
        with owner.call(boundary.operation, original, parameters=boundary.parameters, work=boundary.work,
                        device=boundary.input_use == "device") as call:

            # Snapshot mutation epochs before native work; an early return may later discard unused ports.
            inputs = {name: owner.tensors.observe(tensor, shape) for name, (tensor, shape) in boundary.inputs.items()}
            yield
            call.record["parameters"].update(boundary.parameters)
            call.record["work"]["context"] = boundary.work["context"]
            call.record["bindings"].update(action=True, condition=boundary.condition)
            call.record["guards"] = list(boundary.guards)
            for name in boundary.inputs:
                call.port(name, "input", inputs[name], use=boundary.input_use, at_return=boundary.input_use == "host")
            for name, (tensor, shape) in boundary.outputs.items():
                call.port(name, "output", owner.tensors.observe(tensor, shape), use="device")

    @contextmanager
    def enclosing(self, frame):
        with self.observation.call(
            "invocation", frame.original, parameters=frame.parameters, work=frame.work,
            device=not frame.host_only and frame.operation != "kv_enqueue",
        ) as call:

            # Only the segments contain native business work; this envelope preserves their nesting.
            call.record["envelope"] = True
            yield
        if frame.values:
            self.observation.action_values.append({
                "scope": frame.observations[-1], "values": {**frame.values, "segments": frame.observations},
            })

    @contextmanager
    def fragment(self, frame):
        owner = self.observation
        with owner.call("segment", frame.original, device=not frame.host_only and frame.operation != "kv_enqueue") as call:
            call.record["guards"] = list(frame.guards)
            for name, (tensor, shape) in frame.tensor_inputs.items():
                call.port(name, "input", owner.tensors.observe(tensor, shape), use="device")
            for name, (tensor, shape) in frame.metadata_inputs.items():
                call.port(name, "input", owner.tensors.observe(tensor, shape), use="metadata")
            yield
            call.record["work"]["continuation"] = frame.continuation
            call.record["bindings"]["segment"] = {
                "owner": frame.operation,
            }
            call.record["bindings"]["condition"] = frame.condition
            if frame.host_only:
                call.record["bindings"]["segment"]["host_only"] = True
            for name, (tensor, shape) in frame.tensor_outputs.items():
                call.output(name, owner.tensors.observe(tensor, shape), use="device", preceding=frame.observations)
            if frame.terminal and frame.operation in {"result", "prebuilt_result", "prefill_finish"}:
                call.record["effects"] = frame.outputs
            if frame.terminal and frame.operation == "prebuilt_prepare" and owner.resource_history is not None:
                owner.resource_history.batch(call.record["id"], frame.inputs)
            if frame.terminal and frame.operation == "publication":
                call.record["publication"] = frame.outputs
                call.port("publication", "output", None, use="host")

        # Segment identities also connect deferred tensor outputs when optional diagnostic recording is disabled.
        frame.observations.append(call.identity)

    def result(self, original, scheduler, batch, output):
        owner = self.observation
        if not owner.active:
            return original(scheduler, batch, output)
        work = owner.batch_work(batch)
        parameters = BatchTensors.parameters(batch, sum(member["new_tokens"] for member in work["members"]))
        with self.call("result", original, parameters=parameters, work=work, host_inputs=HostReads.result_inputs(output)) as frame:
            result = original(scheduler, batch, output)

            # A nested prebuilt result already publishes its own completion boundary.
            if not batch.forward_mode.is_prebuilt():
                frame.outputs = self.completed(batch.reqs)
            return result

    def publication(self, original, sender, output, *args, **kwargs):
        from sglang.srt.managers.io_struct import BatchTokenIDOutput

        if self.current.get() is None or not isinstance(output, BatchTokenIDOutput) or sender.socket is None:
            return original(sender, output, *args, **kwargs)
        with self.call("publication", original) as frame:
            frame.outputs = [
                {"id": self.observation.content.aliases[request], "generated_tokens": tokens}
                for request, tokens in zip(output.rids, output.completion_tokens, strict=True)
            ]
            return original(sender, output, *args, **kwargs)

    def build(self, original, *args, **kwargs):
        result = original(*args, **kwargs)
        allocator = result.token_to_kv_pool_allocator
        cache_type = type(result.tree_cache)
        allocator_type = type(allocator)
        allocator._markov_allocation = AllocationABI(f"{allocator_type.__module__}.{allocator_type.__qualname__}")
        self.observation.implementation["allocator"] = allocator._markov_allocation.implementation

        # Record the effective native choice, including model-driven cache disabling.
        self.observation.implementation["cache"] = {
            "type": f"{cache_type.__module__}.{cache_type.__qualname__}",
            "disabled": result.disable_radix_cache,
            "eviction_policy": kwargs["server_args"].radix_eviction_policy.lower(),
        }

        # A PD receiver can finish without a model call; pool geometry belongs to construction, not attention.
        self.observation.kv_layout = MhaKvLayout.from_pool(allocator.get_kvcache())
        self.observation.prepare_roles()
        if self.observation.resource_history is not None:
            self.observation.resource_history.bind(result.tree_cache, allocator, result.req_to_token_pool)

        # Diagnostics retain original readers before semantic cuts are installed; snapshots cannot create queries.
        self.resources.bind(result.tree_cache, allocator, result.req_to_token_pool)
        return result

    def install(self) -> None:
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        for target, hook in (
            ("sglang.srt.mem_cache.kv_cache_builder.build_kv_cache", self.build),
            ("sglang.srt.managers.scheduler.Scheduler.process_batch_result", self.result),
            ("sglang.srt.managers.scheduler_components.output_sender.SenderWrapper.send_output", self.publication),
        ):
            HookRegistry.register(target, hook, HookType.AROUND)
        self.selection.install()
        DecisionClocks(self).install()
        HostReads(self).install()
        MetadataCollectives(self).install()
        if self.observation.description["mode"] != "null":
            DisaggregatedCalls(self).install(self.observation.description["mode"])
