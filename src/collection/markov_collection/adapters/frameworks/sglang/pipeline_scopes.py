"""Observe native pipeline actions; the framework keeps microbatch rotation and event ownership."""

from markov_adapters.sglang.batches import BatchTensors
from markov_adapters.sglang.pipeline import PipelineABI, PipelineMessages

from markov_collection.adapters.runtimes.pytorch.signals import NativeSignals


class PipelineScopes:
    def __init__(self, observation) -> None:
        self.observation = observation
        self.messages = PipelineMessages(observation.calls)
        self.signals = NativeSignals(observation.capture, self.messages)

    def launch(self, original, scheduler, mb_id, pp_proxy_tensors, mb_metadata, last_rank_comm_queue):
        owner = self.observation
        if not owner.active:
            return original(scheduler, mb_id, pp_proxy_tensors, mb_metadata, last_rank_comm_queue)

        parameters, work = owner.model_work(scheduler.cur_batch)
        with owner.call(
            "launch", original, parameters=parameters, work=work, device=True, stream=scheduler.forward_stream,
        ) as call, self.signals.observe(call):
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
        with owner.call("output_skip", original, parameters=parameters, work=work, device=True) as call, self.signals.observe(call):
            outputs, result, event = original(scheduler, batch, mb_metadata)
            owner.result_outputs(call, result)
            self.signals.publish(call, event, "completion")
            return outputs, result, event

    def wait_stream(self, original, stream, other):
        owner = self.observation
        if not owner.active or owner.capture.current.get() != 0:
            return original(stream, other)
        streams = PipelineABI.streams(owner.scheduler, stream, other)
        work = self.messages.work()
        work["context"]["streams"] = streams
        with owner.capture.call(
            "stream_wait", original.__qualname__, f"{original.__module__}.{original.__qualname__}",
            parameters={}, work=work, bindings={"streams": streams}, device=True, stream=stream,
        ):
            result = original(stream, other)
        owner.capture.runtime.retire()
        return result

    def install(self):
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        from markov_collection.adapters.frameworks.sglang.transfers import NativeTransfers

        self.messages.install()
        NativeTransfers(self.observation, self.messages).install()
        HookRegistry.register(
            "sglang.srt.managers.scheduler.Scheduler._pp_launch_batch", self.launch, HookType.AROUND,
        )
        HookRegistry.register(
            "sglang.srt.managers.scheduler.Scheduler._pp_prep_batch_result", self.prepare, HookType.AROUND,
        )
        HookRegistry.register(
            "sglang.srt.managers.scheduler.Scheduler._pp_make_skip_output_result", self.skip_output, HookType.AROUND,
        )
        for name in ("record", "wait", "synchronize", "query"):
            HookRegistry.register(f"torch.npu.Event.{name}", getattr(self.signals, name), HookType.AROUND)
        HookRegistry.register("torch.npu.Stream.wait_stream", self.wait_stream, HookType.AROUND)
