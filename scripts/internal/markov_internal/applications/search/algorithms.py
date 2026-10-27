"""Budget-independent, reproducible index streams over one shared search space."""

from __future__ import annotations

from collections.abc import Iterator
from itertools import zip_longest
import random

from markov_internal.applications.search.space import SearchSpace

LOCAL_CENTERS = 4
LOCAL_NEIGHBORS_PER_CENTER = 6


class Permutation:
    """Lazy Fisher-Yates: memory is proportional to draws, not space size."""

    def __init__(self, count: int, rng: random.Random) -> None:
        self.remaining = count
        self.rng = rng
        self.swaps: dict[int, int] = {}

    def take(self) -> int:
        slot = self.rng.randrange(self.remaining)
        result = self.swaps.get(slot, slot)
        self.remaining -= 1
        self.swaps[slot] = self.swaps.get(self.remaining, self.remaining)
        return result


def candidate_indices(
    space: SearchSpace, algorithm: str, seed: int | None, observations: dict[int, float | None] | None = None
) -> Iterator[int]:
    """Static streams have stable prefixes; local search uses prior model scores only."""

    if algorithm in {"local", "coordinate"}:
        observations = observations or {}
        ranked = sorted((i for i, score in observations.items() if score is not None), key=observations.__getitem__)
        seen = set(observations)
        centers = {}
        for index in ranked:
            branch = space.branches[space.branch_index(index)]
            centers.setdefault((branch.page_size, branch.write_policy, branch.prefetch_policy), index)
            if len(centers) == LOCAL_CENTERS:
                break
        rng = random.Random(seed)
        groups = []
        for center in centers.values():
            neighbors = [i for i in space.neighbors(center, joint=algorithm == "local") if i not in seen]
            rng.shuffle(neighbors)
            groups.append(neighbors[:LOCAL_NEIGHBORS_PER_CENTER])
        for group in zip_longest(*groups):
            for index in group:
                if index is not None and index not in seen:
                    seen.add(index)
                    yield index
        for index in candidate_indices(space, "stratified_random", seed):
            if index not in seen:
                yield index
        return

    if algorithm == "enumerate":
        yield from range(space.count)
        return

    rng = random.Random(seed)
    if algorithm == "random":
        points = Permutation(space.count, rng)
        while points.remaining:
            yield points.take()
        return

    branches = [Permutation(branch.count, rng) for branch in space.branches]
    order = Permutation(len(branches), rng)
    while order.remaining:
        number = order.take()
        yield space.offsets[number] + branches[number].take()

    remaining = sum(branch.remaining for branch in branches)
    while remaining:
        slot = rng.randrange(remaining)
        for number, branch in enumerate(branches):
            if slot < branch.remaining:
                yield space.offsets[number] + branch.take()
                remaining -= 1
                break
            slot -= branch.remaining
