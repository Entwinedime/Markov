"""Native PP boundary names shared by observation and control."""

from contextvars import ContextVar
from functools import partial


class PipelineMessages:
    def __init__(self, calls):
        self.calls = calls
        self.message = ContextVar("pipeline_message", default=None)

    def work(self):
        work = self.calls.work(None)
        work["context"] = {
            "pipeline": PipelineABI.control_context(self.calls),
            "message": None if self.message.get() is None else dict(self.message.get()),
        }
        return work

    def signal_work(self, stream):
        work = self.work()
        work["context"]["record_stream"] = PipelineABI.stream_role(self.calls.scheduler, stream)
        return work

    def wait_work(self, producer, *, device, stream=None):
        work = self.work()
        work["context"].update(producer=producer, device_wait=device)
        if device:
            work["context"]["wait_stream"] = PipelineABI.stream_role(self.calls.scheduler, stream)
        return work

    def dictionary(self, direction, original, group, *args, **kwargs):
        if not self.calls.enabled or group.world_size == 1:
            return original(group, *args, **kwargs)
        previous = self.message.set({"direction": direction, "kind": None, "field": None})
        try:
            return original(group, *args, **kwargs)
        finally:
            self.message.reset(previous)

    def split(self, original, tensor_dict):
        metadata, tensors = original(tensor_dict)
        message = self.message.get()
        if message is None or message["direction"] != "send":
            return metadata, tensors

        from sglang.srt.distributed.parallel_state import TensorMetadata

        message["kind"] = tensor_dict.get("__msg_type__")
        names = [name for name, value in metadata if isinstance(value, TensorMetadata)]
        return metadata, self.fields(zip(names, tensors, strict=True), paired=False)

    def receive_metadata(self, original, group, *args, **kwargs):
        metadata = original(group, *args, **kwargs)
        message = self.message.get()
        if message is None or message["direction"] != "receive":
            return metadata
        message["kind"] = dict(metadata).get("__msg_type__")
        return self.fields(metadata, paired=True)

    def fields(self, entries, *, paired):

        # Attribute the native loop's current field, including equal-shaped tensors and sliced views.
        for name, value in entries:
            self.message.get()["field"] = name
            yield (name, value) if paired else value

    def install(self):
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        module = "sglang.srt.distributed.parallel_state."
        for target, hook in (
            (module + "GroupCoordinator.send_tensor_dict", partial(self.dictionary, "send")),
            (module + "GroupCoordinator.recv_tensor_dict", partial(self.dictionary, "receive")),
            (module + "_split_tensor_dict", self.split),
            (module + "GroupCoordinator.recv_object", self.receive_metadata),
        ):
            HookRegistry.register(target, hook, HookType.AROUND)


class PipelineABI:
    @staticmethod
    def control_context(calls):
        scheduler = calls.scheduler

        # Fixed native slots identify placement, not polling rounds; absent and empty batches remain distinct.
        return {
            "current": None if scheduler.cur_batch is None else calls.progress(scheduler.cur_batch.reqs),
            "slots": [None if batch is None else calls.progress(batch.reqs) for batch in scheduler.mbs],
            "running_slots": [index for index, batch in enumerate(scheduler.running_mbs) if batch is scheduler.running_batch],
        }

    @staticmethod
    def proxy_tensors(proxy):
        import torch

        # Native PP dictionaries also retain routing metadata such as the message kind.
        return {name: value for name, value in proxy.tensors.items() if isinstance(value, torch.Tensor)}

    @staticmethod
    def tensor_parameters(tensor):
        return {f"extent_{axis}": size for axis, size in enumerate(tensor.shape)}

    @staticmethod
    def gather_condition(tensor, dim, group):
        return {"dtype": str(tensor.dtype), "dim": dim % tensor.ndim, "group": group}

    @staticmethod
    def streams(scheduler, target, source):
        return {
            direction: PipelineABI.stream_role(scheduler, stream)
            for direction, stream in (("source", source), ("target", target))
        }

    @staticmethod
    def stream_role(scheduler, stream):
        stream = scheduler.device_module.current_stream() if stream is None else stream
        return next(role for role in ("schedule", "forward", "copy") if stream == getattr(scheduler, f"{role}_stream"))
