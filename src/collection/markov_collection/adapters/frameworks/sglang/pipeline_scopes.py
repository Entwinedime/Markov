"""Observe native pipeline actions; the framework keeps microbatch rotation and event ownership."""

from markov_adapters.sglang.batches import BatchTensors
from markov_adapters.sglang.pipeline import PipelineABI, PipelineMessages

from markov_collection.adapters.frameworks.sglang.transfers import HostTransfers, NativeTransfers
from markov_collection.adapters.runtimes.pytorch.signals import NativeSignals


class PipelineCuts:
    def __init__(self, calls, current, runtime=None) -> None:
        self.calls = calls
        self.current = current
        self.messages = PipelineMessages(calls)
        self.signals = NativeSignals(current, self.site_scope, self.messages, runtime)

    def wait_stream(self, original, stream, other):
        if not self.calls.enabled or self.current.get() != 0:
            return original(stream, other)
        streams = PipelineABI.streams(self.calls.scheduler, stream, other)
        work = self.messages.work()
        work["context"]["streams"] = streams
        with self.site_scope("stream_wait", original, work=work, bindings={"streams": streams}, device=True, stream=stream):
            return original(stream, other)

    def install(self):
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        self.messages.install()
        self.transfers.install()
        scheduler = "sglang.srt.managers.scheduler.Scheduler."
        for name, hook in (
            ("_pp_launch_batch", self.launch),
            ("_pp_prep_batch_result", self.prepare),
            ("_pp_make_skip_output_result", self.skip_output),
        ):
            HookRegistry.register(scheduler + name, hook, HookType.AROUND)
        for name in ("record", "wait", "synchronize", "query"):
            HookRegistry.register(f"torch.npu.Event.{name}", getattr(self.signals, name), HookType.AROUND)
        HookRegistry.register("torch.npu.Stream.wait_stream", self.wait_stream, HookType.AROUND)


class PipelineScopes(PipelineCuts):
    def __init__(self, observation) -> None:
        super().__init__(observation.calls, observation.capture.current, observation.capture.runtime)
        self.observation = observation
        self.transfers = NativeTransfers(observation, self.messages)

    def site_scope(self, role, original, *, work, device, stream, bindings=None):
        return self.observation.capture.call(
            role, original.__qualname__, f"{original.__module__}.{original.__qualname__}", parameters={},
            work=work, bindings=bindings, device=device, stream=stream,
        )

    def launch(self, original, scheduler, mb_id, pp_proxy_tensors, mb_metadata, last_rank_comm_queue):
        owner = self.observation
        if not owner.active:
            return original(scheduler, mb_id, pp_proxy_tensors, mb_metadata, last_rank_comm_queue)

        parameters, work = owner.model_work(scheduler.cur_batch)
        with owner.call(
            "launch", original, parameters=parameters, work=work, device=True, stream=scheduler.forward_stream,
        ) as call, self.signals.observe():
            if pp_proxy_tensors is not None:
                for name, tensor in PipelineABI.proxy_tensors(pp_proxy_tensors).items():

                    # Launch forwards handles before its stream wait; the model body owns the actual device read.
                    call.port(f"proxy.{name}", "input", owner.tensors.observe(tensor, owner.modules.proxy_shape(tensor)), use="metadata")
            result, event = original(scheduler, mb_id, pp_proxy_tensors, mb_metadata, last_rank_comm_queue)
            self.signals.publish(call, event, "completion")
            owner.result_outputs(call, result)
            return result, event

    def prepare(self, original, scheduler, batch, mb_metadata, pp_outputs):
        owner = self.observation
        if not owner.active:
            return original(scheduler, batch, mb_metadata, pp_outputs)
        work = owner.batch_work(batch)
        parameters = BatchTensors.parameters(batch, sum(member["new_tokens"] for member in work["members"]))
        with owner.request_state.members(batch), owner.call("output_prepare", original, parameters=parameters, work=work, device=True, stream=scheduler.copy_stream) as call:
            call.port("next_token_ids", "input", owner.tensors.observe(pp_outputs["next_token_ids"], ["members"]), use="device")
            result = original(scheduler, batch, mb_metadata, pp_outputs)
            owner.result_outputs(call, result)
            return result

    def skip_output(self, original, scheduler, batch, mb_metadata):
        owner = self.observation
        if not owner.active:
            return original(scheduler, batch, mb_metadata)
        work = owner.batch_work(batch)
        parameters = BatchTensors.parameters(batch, sum(member["new_tokens"] for member in work["members"]))
        with owner.call("output_skip", original, parameters=parameters, work=work, device=True) as call, self.signals.observe():
            outputs, result, event = original(scheduler, batch, mb_metadata)
            owner.result_outputs(call, result)
            self.signals.publish(call, event, "completion")
            return outputs, result, event


class HostPipelineScopes(PipelineCuts):
    def __init__(self, host) -> None:
        super().__init__(host, host.parent)
        self.transfers = HostTransfers(host, self.messages)

    def site_scope(self, role, original, *, work, device, stream, bindings=None):
        return self.calls.native_call(role, original, work=work, bindings=bindings)

    def launch(self, original, scheduler, mb_id, pp_proxy_tensors, mb_metadata, last_rank_comm_queue):
        args = scheduler, mb_id, pp_proxy_tensors, mb_metadata, last_rank_comm_queue
        if not self.calls.enabled:
            return original(*args)
        work = self.calls.batch_work(scheduler.cur_batch)
        with self.calls.native_call("launch", original, work=work) as call, self.signals.observe():
            result, event = original(*args)
            self.signals.publish(call, event, "completion")
            return result, event

    def prepare(self, original, scheduler, batch, mb_metadata, pp_outputs):
        if not self.calls.enabled:
            return original(scheduler, batch, mb_metadata, pp_outputs)
        with self.calls.native_call("output_prepare", original, work=self.calls.batch_work(batch)):
            return original(scheduler, batch, mb_metadata, pp_outputs)

    def skip_output(self, original, scheduler, batch, mb_metadata):
        if not self.calls.enabled:
            return original(scheduler, batch, mb_metadata)
        work = self.calls.batch_work(batch)
        with self.calls.native_call("output_skip", original, work=work) as call, self.signals.observe():
            outputs, result, event = original(scheduler, batch, mb_metadata)
            self.signals.publish(call, event, "completion")
            return outputs, result, event
