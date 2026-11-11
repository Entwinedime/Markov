"""Describe explicit CPU point-to-point calls using each native world's actual ranks."""

import torch.distributed as distributed


class CpuTransfer:
    def __init__(self, world) -> None:
        self.world = world

    def describe(self, tensor, group, tag, endpoint, group_endpoint, receiving):
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
        }
