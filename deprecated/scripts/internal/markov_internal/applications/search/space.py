"""Index a finite, conditional HiCache space without materializing every target."""

from __future__ import annotations

from bisect import bisect_right
from dataclasses import dataclass
from itertools import product
import math
from typing import Any

from markov_internal.modeling.cost.io_contract import positive_u64
from markov_internal.modeling.sglang_config import (
    TIMEOUT_FIELDS,
    canonical_hicache,
    canonical_timeout,
    resolve_hicache_fields,
)
from markov_internal.modeling.types import TargetHiCacheConfig

INTEGER_FIELDS = ("page_size", "l1_capacity_pages", "l2_capacity_pages", "prefetch_threshold_pages")
TOKEN_FIELDS = {name: name.removesuffix("_pages") + "_tokens" for name in INTEGER_FIELDS if name != "page_size"}
POLICIES = {
    "write_policy": ("write_through", "write_through_selective", "write_back"),
    "prefetch_policy": ("wait_complete", "best_effort", "timeout"),
}


@dataclass(frozen=True)
class CapacityPairs:
    device: tuple[int, ...]
    host: tuple[int, ...]
    offsets: tuple[int, ...]

    @classmethod
    def create(cls, device: tuple[int, ...], host: tuple[int, ...]) -> CapacityPairs:
        offsets = [0]
        for pages in device:
            offsets.append(offsets[-1] + len(host) - bisect_right(host, pages))

        return cls(device, host, tuple(offsets))

    @property
    def count(self) -> int:
        return self.offsets[-1]

    def at(self, index: int) -> tuple[int, int]:
        group = bisect_right(self.offsets, index) - 1
        device = self.device[group]
        host_index = bisect_right(self.host, device) + index - self.offsets[group]
        return device, self.host[host_index]

    def locate(self, device: int, host: int) -> int | None:
        if device not in self.device or host not in self.host or host <= device:
            return None

        group = self.device.index(device)
        return self.offsets[group] + self.host.index(host) - bisect_right(self.host, device)


@dataclass(frozen=True)
class Branch:
    page_size: int
    write_policy: str
    prefetch_policy: str
    capacities: CapacityPairs
    axes: tuple[tuple[Any, ...], ...]

    @property
    def count(self) -> int:
        return self.capacities.count * math.prod(len(axis) for axis in self.axes)

    @property
    def description(self) -> dict[str, Any]:
        return {
            "page_size": self.page_size,
            "write_policy": self.write_policy,
            "prefetch_policy": self.prefetch_policy,
            "configurations": self.count,
        }


@dataclass(frozen=True)
class SearchSpace:
    template: TargetHiCacheConfig
    branches: tuple[Branch, ...]
    offsets: tuple[int, ...]
    invalid_capacity_combinations: int
    token_fields: frozenset[str]

    @classmethod
    def from_raw(cls, template: TargetHiCacheConfig, raw: dict[str, Any]) -> SearchSpace:
        """Admit finite axes; token capacities stay fixed as page size changes."""

        allowed = {*INTEGER_FIELDS, *TOKEN_FIELDS.values(), *POLICIES, "timeout"}
        if set(raw) - allowed:
            raise ValueError("search variables are page/token capacities, policies, threshold, and conditional timeout")
        if any(name in raw and token in raw for name, token in TOKEN_FIELDS.items()):
            raise ValueError("choose pages or tokens for each capacity/threshold axis")
        fields = template.fields
        if set(fields) - {"enabled", *INTEGER_FIELDS, *POLICIES, *TIMEOUT_FIELDS, "prefetch_capacity_limit_pages"}:
            raise ValueError("the search target template contains unsupported fields")
        if fields.get("enabled", True) is not True:
            raise ValueError("HiCache search requires an enabled target template")

        def axis(name: str, declarations: dict[str, Any]) -> tuple[Any, ...]:
            values = declarations[name] if name in declarations else [fields[name]]
            if not isinstance(values, list):
                raise ValueError(f"search {name} needs a finite list")

            for value in values:
                if name in INTEGER_FIELDS or name in TOKEN_FIELDS.values():
                    positive_u64(value, name)
                elif name in POLICIES:
                    if value not in POLICIES[name]:
                        raise ValueError(f"unsupported {name}: {value}")
                elif (
                    isinstance(value, bool)
                    or not isinstance(value, (int, float))
                    or not math.isfinite(value)
                    or value < 0
                ):
                    raise ValueError(f"{name} needs finite nonnegative seconds")

            return tuple(dict.fromkeys(values))

        axes = {name: axis(name, raw) for name in (*INTEGER_FIELDS, *POLICIES)}
        tokens = {name: axis(token, raw) for name, token in TOKEN_FIELDS.items() if token in raw}
        timeout = raw.get("timeout", {})
        if not isinstance(timeout, dict) or set(timeout) - {*TIMEOUT_FIELDS, "profiles"}:
            raise ValueError("timeout accepts parameter lists or explicit profiles")
        if "profiles" in timeout and set(timeout) != {"profiles"}:
            raise ValueError("timeout profiles and independent parameter lists are mutually exclusive")

        profiles = {}
        if "timeout" in axes["prefetch_policy"]:
            if "profiles" in timeout:
                if not isinstance(timeout["profiles"], list):
                    raise ValueError("timeout.profiles must be a list of the three timeout fields")
                declarations = timeout["profiles"]
            else:
                declarations = (
                    dict(zip(TIMEOUT_FIELDS, values))
                    for values in product(*(axis(name, timeout) for name in TIMEOUT_FIELDS))
                )
            for declaration in declarations:
                if not isinstance(declaration, dict) or set(declaration) != set(TIMEOUT_FIELDS):
                    raise ValueError("each timeout profile must declare all three timeout fields")
                profile = tuple(axis(name, {name: [declaration[name]]})[0] for name in TIMEOUT_FIELDS)
                profiles.setdefault(canonical_timeout(*profile), profile)

        branches = []
        offsets = [0]
        invalid = 0
        for page, write, prefetch in product(axes["page_size"], axes["write_policy"], axes["prefetch_policy"]):
            page_axes = dict(axes)
            for name, values in tokens.items():
                if any(value % page for value in values):
                    raise ValueError(f"{TOKEN_FIELDS[name]} must be divisible by every selected page size")
                page_axes[name] = tuple(value // page for value in values)

            capacities = CapacityPairs.create(
                tuple(sorted(page_axes["l1_capacity_pages"])), tuple(sorted(page_axes["l2_capacity_pages"]))
            )
            active_axes = (
                page_axes["prefetch_threshold_pages"],
                *((tuple(profiles.values()),) if prefetch == "timeout" else ()),
            )
            branch = Branch(page, write, prefetch, capacities, active_axes)
            invalid += (len(capacities.device) * len(capacities.host) - capacities.count) * math.prod(
                map(len, active_axes)
            )
            if branch.count:
                branches.append(branch)
                offsets.append(offsets[-1] + branch.count)

        return cls(template, tuple(branches), tuple(offsets), invalid, frozenset(tokens))

    @property
    def count(self) -> int:
        return self.offsets[-1]

    def branch_index(self, index: int) -> int:
        return bisect_right(self.offsets, index) - 1

    def target(self, index: int, label: str | None = None) -> TargetHiCacheConfig:
        branch_index = self.branch_index(index)
        branch = self.branches[branch_index]
        position = index - self.offsets[branch_index]
        coordinates = []
        for size in reversed((branch.capacities.count, *(len(axis) for axis in branch.axes))):
            position, coordinate = divmod(position, size)
            coordinates.append(coordinate)
        coordinates.reverse()

        device, host = branch.capacities.at(coordinates[0])
        fields = {
            **self.template.fields,
            "page_size": branch.page_size,
            "write_policy": branch.write_policy,
            "prefetch_policy": branch.prefetch_policy,
            "l1_capacity_pages": device,
            "l2_capacity_pages": host,
            "prefetch_threshold_pages": branch.axes[0][coordinates[1]],
        }
        if branch.prefetch_policy == "timeout":
            fields.update(zip(TIMEOUT_FIELDS, branch.axes[1][coordinates[2]]))

        return TargetHiCacheConfig.from_raw(
            {"name": label or f"candidate_{index}", "hicache": resolve_hicache_fields(fields)}
        )

    def locate(self, fields: dict[str, Any]) -> int | None:
        """Find an exact execution point, for charging/reusing a self reference once."""

        for number, branch in enumerate(self.branches):
            if (branch.page_size, branch.write_policy, branch.prefetch_policy) != (
                fields["page_size"],
                fields["write_policy"],
                fields["prefetch_policy"],
            ):
                continue

            capacity = branch.capacities.locate(fields["l1_capacity_pages"], fields["l2_capacity_pages"])
            values = (fields["prefetch_threshold_pages"],)
            if branch.prefetch_policy == "timeout":
                profile = canonical_timeout(*(fields[name] for name in TIMEOUT_FIELDS))
                stored = next((p for p in branch.axes[1] if canonical_timeout(*p) == profile), None)
                if stored is None:
                    continue
                values += (stored,)
            if capacity is None or any(value not in axis for value, axis in zip(values, branch.axes)):
                continue

            position = capacity
            for value, axis in zip(values, branch.axes):
                position = position * len(axis) + axis.index(value)
            index = self.offsets[number] + position
            if canonical_hicache(self.target(index).fields) == canonical_hicache(fields):
                return index

        return None

    def neighbors(self, index: int, *, joint: bool = True) -> tuple[int, ...]:
        """Legal single-axis moves, optionally including two declared paired moves."""

        current = self.target(index).fields
        branch = self.branches[self.branch_index(index)]

        def adjacent(values: tuple[Any, ...], value: Any) -> list[Any]:
            ordered = sorted(set(values))
            at = ordered.index(value)
            return ordered[max(0, at - 1) : at] + ordered[at + 1 : at + 2]

        axes = {
            "page_size": tuple(b.page_size for b in self.branches),
            "write_policy": tuple(dict.fromkeys(b.write_policy for b in self.branches)),
            "prefetch_policy": tuple(dict.fromkeys(b.prefetch_policy for b in self.branches)),
            "l1_capacity_pages": branch.capacities.device,
            "l2_capacity_pages": branch.capacities.host,
            "prefetch_threshold_pages": branch.axes[0],
        }
        choices = {
            name: [v for v in values if v != current[name]] if name in POLICIES else adjacent(values, current[name])
            for name, values in axes.items()
        }
        if branch.prefetch_policy == "timeout":
            choices.update(
                {
                    name: adjacent(tuple(p[i] for p in branch.axes[1]), current[name])
                    for i, name in enumerate(TIMEOUT_FIELDS)
                }
            )

        moves = [{name: value} for name, values in choices.items() for value in values]
        if joint:
            for first, second in (("page_size", "prefetch_threshold_pages"), ("write_policy", "prefetch_policy")):
                moves.extend({first: a, second: b} for a, b in product(choices[first], choices[second]))
        neighbors = set()
        for move in moves:
            fields = {**current, **move}
            if "page_size" in move:
                for name in self.token_fields:
                    fields[name] = fields[name] * current["page_size"] // fields["page_size"]
            if fields["l2_capacity_pages"] <= fields["l1_capacity_pages"]:
                continue
            found = self.locate(resolve_hicache_fields(fields))
            if found is not None and found != index:
                neighbors.add(found)

        return tuple(sorted(neighbors))
