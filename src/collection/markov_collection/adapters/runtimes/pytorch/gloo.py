"""Necessary completion entries for the pinned Gloo binomial-tree broadcast."""

from __future__ import annotations

from collections.abc import Sequence
from typing import Any


class GlooBroadcastLayout:
    @staticmethod
    def completion_entry_dependencies(group: Any, ranks: Sequence[int], root: int) -> list[list[int]] | None:
        import torch.distributed as dist

        if dist.get_backend(group) != "gloo":
            return None

        size = len(ranks)
        root_index = ranks.index(root)
        children: list[list[int]] = [[] for _ in ranks]
        for rank in range(1, size):
            parent = rank ^ (1 << (rank.bit_length() - 1))
            children[parent].append(rank)

        dependencies = []
        for completion in range(size):
            entries = {completion, *children[completion]}
            ancestor = completion
            while ancestor:
                ancestor ^= 1 << (ancestor.bit_length() - 1)
                entries.add(ancestor)

            # Rotate group indices around the actual root. Entries constrain
            # completion, without claiming a receive-post or worker-start time.
            for entry in entries:
                dependencies.append([ranks[(entry + root_index) % size], ranks[(completion + root_index) % size]])

        dependencies.sort()
        return dependencies
