"""Separate CPU transfer identity from its physical wire features."""


class CpuTransfer:
    def __init__(self, world) -> None:
        self.world = world

    @staticmethod
    def identity(description):

        # Private object serialization can change byte counts without changing the native control operation.
        return {name: description[name] for name in ("group", "source", "target", "tag", "receiving", "dtype")}

    def describe(self, tensor, group, tag, endpoint, group_endpoint, receiving):
        import torch.distributed as distributed

        group = distributed.group.WORLD if group is None else group
        if group_endpoint is not None:
            endpoint = distributed.get_global_rank(group, group_endpoint)
        if endpoint is None:
            raise ValueError("Native metadata receive requires an explicit sender")
        local = distributed.get_rank()
        source, target = (endpoint, local) if receiving else (local, endpoint)
        return {
            "group": group.group_name, "source": self.world[source], "target": self.world[target],
            "tag": tag, "receiving": receiving, "dtype": str(tensor.dtype), "shape": list(tensor.shape),
            "elements": tensor.numel(), "element_bytes": tensor.element_size(),
        }
