"""Share CPU collective execution cuts between native observation and control."""

import inspect
from functools import partial

from markov_adapters.sglang.calls import ActionCall


class MetadataCollectives:
    def __init__(self, calls) -> None:
        self.calls = calls

    @staticmethod
    def snapshot(arguments, root):
        import torch.distributed as distributed

        if "input_tensor" in arguments:
            inputs, outputs = arguments["input_tensor"], arguments["output_tensor"]
        else:
            inputs = outputs = arguments["tensor"]

        # A broadcast receiver's pre-call buffer is uninitialized storage, not an operand.
        values = [] if root is not None and distributed.get_rank(arguments["group"]) != root else inputs.flatten().tolist()
        return {"input": values}, outputs

    def invoke(self, name, signature, original, *args, **kwargs):
        calls = self.calls
        parent = calls.current.get()
        if not calls.enabled or parent is None:
            return original(*args, **kwargs)

        import torch.distributed as distributed

        arguments = signature.bind(*args, **kwargs)
        arguments.apply_defaults()
        values = arguments.arguments
        tensor = values["input_tensor" if name == "all_gather_into_tensor" else "tensor"]
        group = distributed.group.WORLD if values["group"] is None else values["group"]
        if not calls.groups.is_cpu(group):
            return calls.device_collective(original, values)

        # Empty Gloo reductions and singleton groups do not impose a peer rendezvous.
        if distributed.get_world_size(group) == 1 or (name == "all_reduce" and tensor.numel() == 0):
            return original(*args, **kwargs)
        if values["async_op"]:
            raise ValueError("CPU metadata collectives require blocking completion")

        members = distributed.get_process_group_ranks(group)
        root = None
        if name == "broadcast":
            source = values.get("group_src")
            root = members.index(values["src"]) if source is None else source
        collective = {
            "group": calls.groups.name(group), "operation": name, "dtype": str(tensor.dtype), "root": root,
        }
        if name == "all_reduce":
            collective["reduction"] = str(values["op"])
        work = {**parent.work, "context": {**parent.work.get("context", {}), "collective": collective}}
        boundary = ActionCall(
            "metadata_collective", {"elements": tensor.numel()}, work, condition={"collective": collective},
        )

        # Submit preceding CPU work before rendezvous; the native call remains the sole owner of metadata values.
        with calls.separate(boundary.operation, work):
            return calls.collective(original, boundary, values)

    def install(self):
        import torch.distributed as distributed
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        for name in ("broadcast", "all_gather_into_tensor", "all_reduce"):
            hook = partial(self.invoke, name, inspect.signature(getattr(distributed, name)))
            HookRegistry.register(f"torch.distributed.{name}", hook, HookType.AROUND)
