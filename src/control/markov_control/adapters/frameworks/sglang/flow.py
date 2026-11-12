"""Let native lifecycle calls drive interleaved execution and resource operations."""

from contextlib import contextmanager

from markov_adapters.sglang.calls import CallSegments
from markov_adapters.sglang.clocks import DecisionClocks
from markov_adapters.sglang.disaggregated_calls import DisaggregatedCalls
from markov_adapters.sglang.groups import ParallelGroups
from markov_adapters.sglang.host_reads import HostReads
from markov_adapters.sglang.metadata_collectives import MetadataCollectives
from markov_adapters.sglang.pipeline import PipelineABI
from markov_adapters.sglang.resources import ResourceCalls
from markov_adapters.sglang.selection import BatchSelection


class PublishedRequests:
    def __init__(self, calls, enabled: bool) -> None:
        self.calls = calls
        self.enabled = enabled

    def send_output(self, output, *args, **kwargs):
        if not self.enabled or self.calls.current.get() is None:
            return
        with self.calls.call("publication", self.send_output) as frame:

            # Prefill's response acknowledges its transfer; only Decode publishes the serving output.
            if self.calls.scheduler.server_args.disaggregation_mode == "prefill":
                return
            frame.outputs = [
                {
                    "request": int(request), "generated_tokens": tokens,
                    "finished": reason is not None, "emits_output": True,
                }
                for request, tokens, reason in zip(output.rids, output.completion_tokens, output.finished_reasons, strict=True)
            ]


class FlowExecution(CallSegments):
    def __init__(self, scheduler) -> None:
        super().__init__()
        self.scheduler = scheduler
        self.started = False
        self.pending_inputs = {str(index) for index in range(len(scheduler.request_input.requests))}
        self.request_state = scheduler.request_state
        self.groups = ParallelGroups()
        self.clocks = DecisionClocks(self)
        self.clocks.install()
        HostReads(self).install()
        self.selection = BatchSelection(self)
        self.selection.install()
        self.resources = ResourceCalls(self)
        self.resources.bind(scheduler.tree_cache, scheduler.token_to_kv_pool_allocator, scheduler.req_to_token_pool)
        scheduler.output_streamer.send_to_detokenizer = PublishedRequests(
            self, scheduler.output_streamer.send_to_detokenizer.socket is not None,
        )
        if scheduler.server_args.disaggregation_mode != "null":
            DisaggregatedCalls(self).install(scheduler.server_args.disaggregation_mode)

    @property
    def enabled(self):
        return self.started

    def input(self, request):
        self.pending_inputs.remove(request.rid)
        self.started = True

    def drained(self):

        # Close at the same local idle boundary as capture, never between future arrivals or inside a call.
        if not self.pending_inputs and self.current.get() is None:
            self.started = False

    def request_id(self, native_id):
        return self.scheduler.request_input.requests[int(native_id)]["id"]

    def work(self, batch, members=None, *, phase="prefill"):
        scheduler = self.scheduler
        work = scheduler.batch_names.describe(batch)
        if batch is not None:
            work["context"] = {"members": self.progress(batch.reqs)}
        if members is not None:
            work.update(
                phase=phase, rank=scheduler.graph_rank,
                members=[{**member, "id": self.request_id(member["id"])} for member in members],
            )
        return work

    @staticmethod
    def completed(requests):
        return [
            {"request": int(req.rid), "generated_tokens": len(req.output_ids), "finished": req.finished(), "emits_output": False}
            for req in requests
        ]

    def select(self, original, scheduler, *args, **kwargs):
        return self.selection.select(original, scheduler, *args, **kwargs)

    def collective(self, original, boundary, arguments):

        # Inputs are available at graph entry; consuming a result must not wait for peers' graph returns.
        work = original(**{**arguments, "async_op": True})
        returned = self.scheduler.action(boundary.operation, boundary.parameters, boundary.work, condition=boundary.condition)

        # Native Work publishes into the tensor at wait; snapshot only when execution tracing requests evidence.
        values, outputs = (
            MetadataCollectives.snapshot(arguments, boundary.condition["collective"]["root"])
            if "observation" in returned else (None, None)
        )
        work.wait()
        if values is not None:
            values["output"] = outputs.flatten().tolist()
            self.scheduler.peer.call("action_values", boundary=returned["observation"], values={"collective": values})

    def device_collective(self, original, arguments):
        frame = self.current.get()
        if frame.operation != "batch_sync_exchange":
            return super().device_collective(original, arguments)

        # This native method has one exchange and private tensor postprocessing; the whole declared graph owns both.
        work = original(**{**arguments, "async_op": True})
        self.suspend(frame, "call.return")
        work.wait()

    def clock_sample(self, original, query):
        time_ns = self.scheduler.peer.call("clock") if query is None else query["time_ns"]

        # Native policy uses zero as an unset timestamp; a shared positive epoch preserves elapsed durations.
        ticks = time_ns + self.scheduler.clock_epoch_ns
        return ticks / 1_000_000_000, ticks

    @contextmanager
    def read_boundary(self, kind, original, work, condition):

        # Native nonblocking reads complete at this virtual instant, independently of optional tracing.
        returned = self.scheduler.action("query", {}, work, condition=condition)
        query = {"boundary": returned["read_boundary"], "time_ns": returned["completed_ns"]}
        yield query

        self.scheduler.peer.call(
            "read_result", boundary=query["boundary"], result=query["result"], clocks=query["clocks"],
        )

    @contextmanager
    def boundary(self, original, boundary):
        with self.request_state.record() as accesses:
            yield
        self.scheduler.bind_state(boundary.work, accesses)
        self.scheduler.action(
            boundary.operation, boundary.parameters, boundary.work, condition=boundary.condition,
            tensor_inputs={name: tensor for name, (tensor, _) in boundary.inputs.items()},
            tensor_outputs={name: tensor for name, (tensor, _) in boundary.outputs.items()},
        )

    @contextmanager
    def enclosing(self, frame):
        yield
        if frame.values:
            self.scheduler.peer.call(
                "action_values", boundary=frame.observations[-1],
                values={**frame.values, "segments": frame.observations},
            )

    @contextmanager
    def fragment(self, frame):
        with self.request_state.record() as accesses:
            yield

        # Submission facts belong to this segment, not to the caller context inherited by later children.
        work = {**frame.work, "continuation": frame.continuation}
        self.scheduler.bind_state(work, accesses)
        condition = {**frame.condition, "owner": frame.operation}
        if not frame.host_only:
            condition.update(entry=frame.entry, exit=frame.exit)
        outputs = frame.outputs if frame.terminal else ()
        tensors = {name: tensor for name, (tensor, _) in frame.tensor_outputs.items()}
        inputs = {name: tensor for name, (tensor, _) in {**frame.tensor_inputs, **frame.metadata_inputs}.items()}
        returned = self.scheduler.action(
            "segment", frame.parameters, work, outputs, condition=condition, tensor_inputs=inputs, tensor_outputs=tensors,
            allocations=frame.tensor_allocations,
        )
        if self.request_state.observe:
            frame.observations.append(returned["observation"])
        if accesses.values:
            self.scheduler.peer.call("action_values", boundary=returned["observation"], values={"state": [
                {**value, "request": self.request_id(value["request"])} for value in accesses.values
            ]})

    def process(self, original, batch, result):
        parameters, work = self.scheduler.description(batch)
        if self.scheduler.ps.pp_size > 1:
            work["observation_context"] = {"pipeline": PipelineABI.control_context(self)}
        with self.call("result", original, parameters=parameters, work=work, host_inputs=HostReads.result_inputs(result)) as frame:
            original(batch, result)

            # Prebuilt has its own result boundary, also used directly during native Decode selection.
            if not batch.forward_mode.is_prebuilt():
                frame.outputs = self.completed(batch.reqs)
