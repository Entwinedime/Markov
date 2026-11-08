"""Conditions for a contiguous gather buffer's axis move and reshape."""

from typing import Any

import torch


class GatherLayout:
    @staticmethod
    def guard(shape: tuple[int, ...], rule: list[Any], dim: int, peers: int) -> list[Any]:
        dim %= len(shape)
        gathered = torch.empty((peers, *shape), device="meta")
        output_shape = (*shape[:dim], peers * shape[dim], *shape[dim + 1:])
        result = gathered.movedim(0, dim).reshape(output_shape)
        copied = result.untyped_storage()._cdata != gathered.untyped_storage()._cdata

        # Merging the moved group axis with the gathered axis needs a copy only when both strides matter.
        predicate = [
            "and", ["lt", 1, peers], ["lt", 1, ["mul", 1, *rule[:dim]]],
            ["lt", 1, rule[dim]], ["lt", 0, ["mul", 1, *rule]],
        ]
        return predicate if copied else ["eq", predicate, 0]
