"""Declare actual materialization of named native inputs, independently of caller segmentation."""

from markov_adapters.sglang.calls import ActionCall


class HostReads:
    def __init__(self, calls) -> None:
        self.calls = calls

    @staticmethod
    def result_inputs(result):
        tensor = result.next_token_ids
        return {} if tensor is None else {"next_token_ids": (tensor, ["members"])}

    def invoke(self, original, tensor, *args, **kwargs):
        calls = self.calls
        frame = calls.current.get()
        if not calls.enabled or frame is None:
            return original(tensor, *args, **kwargs)
        for name, (value, shape) in frame.host_inputs.items():
            if value is not tensor:
                continue
            work = {**frame.work, "context": {**frame.work["context"], "host_input": name}}
            boundary = ActionCall("host_read", dict(frame.parameters), work, inputs={name: (tensor, shape)},
                                  condition={"input": name, "method": "tolist"}, input_use="host")

            # Only this actual read owns readiness and waiting; unrelated tolist calls remain native.
            with calls.separate(boundary.operation, work), calls.boundary(self.invoke, boundary):
                return original(tensor, *args, **kwargs)
        return original(tensor, *args, **kwargs)

    def install(self):
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        HookRegistry.register("torch.Tensor.tolist", self.invoke, HookType.AROUND)
