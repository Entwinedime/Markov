"""Exact measured distributions and their compact source-position correspondence."""

from __future__ import annotations

from collections import Counter, defaultdict
from collections.abc import Iterable
from dataclasses import dataclass
import json
import math
from pathlib import Path
import sqlite3
from typing import Any
import zlib

from ...common.io import iter_json_lines


def canonical_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"), allow_nan=False)


def _pack(value: Any) -> bytes:
    return zlib.compress(canonical_json(value).encode())


def unpack_samples(value: bytes) -> list[Any]:
    return json.loads(zlib.decompress(value))


@dataclass(frozen=True)
class OperationObservation:
    """One measured operation; capture/position identify evidence, not model variables."""

    capture: str
    manifest: str
    deployment: dict[str, Any]
    position: str
    operation: str
    implementation: str
    features: dict[str, float]
    duration_us: float
    timing_scope: str

    @classmethod
    def from_raw(cls, raw: dict[str, Any]) -> OperationObservation:
        observation = cls(**raw)
        if not observation.capture or not observation.position or not observation.operation or not observation.manifest:
            raise ValueError("cost observations need capture, manifest, operation and source position")
        if observation.duration_us < 0 or not math.isfinite(observation.duration_us):
            raise ValueError("measured duration_us must be finite and non-negative")
        if not observation.timing_scope or any(not math.isfinite(value) for value in observation.features.values()):
            raise ValueError("cost observations need a timing scope and finite features")

        return observation


@dataclass(frozen=True)
class OperationSamples:
    """Exact duration frequencies for one capture, source and complete work point."""

    capture: str
    manifest: str
    operation: str
    implementation: str
    features: dict[str, float]
    timing_scope: str
    durations: dict[float, int]

    @property
    def count(self) -> int:
        return sum(self.durations.values())


def distribution_median(values: dict[float, int]) -> float:
    """The median of the full population, without expanding repeated observations."""

    count = sum(values.values())
    if not count:
        raise ValueError("cannot take the median of an empty measurement distribution")

    lower, upper = (count - 1) // 2, count // 2
    seen = 0
    first = None
    for duration, frequency in sorted(values.items()):
        seen += frequency
        if first is None and seen > lower:
            first = duration
        if seen > upper:
            return duration if lower == upper else (first + duration) / 2

    raise ValueError("measurement frequencies must be positive")


def read_observations(path: Path) -> Iterable[OperationObservation]:
    """Read the public JSON Lines interchange without collecting all rows."""

    for row in iter_json_lines(path):
        yield OperationObservation.from_raw(row)


def operation_rows(document: dict[str, Any]) -> Iterable[dict[str, Any]]:
    """Runtime exports stream; small independent CPU primitives may be inline."""

    from ...common.paths import require_repo_path

    rows = (
        iter_json_lines(require_repo_path(document["operator_observations_file"]))
        if "operator_observations_file" in document
        else document.get("operator_observations", [])
    )
    count = 0
    for row in rows:
        count += 1
        yield row
    if "operator_observation_count" in document and count != document["operator_observation_count"]:
        raise ValueError("Incomplete operation measurement export")


class SampleWriter:
    """Deduplicate one capture at a time inside the caller's SQLite transaction.

    The distribution stores each distinct double and its exact frequency. A
    separate compressed position-to-value index preserves reimport conflicts
    without making training load source strings. Different sources stay distinct.
    """

    def __init__(self, connection: sqlite3.Connection) -> None:
        self.connection = connection
        self.work_ids = {
            (operation, implementation, scope, tuple(sorted(json.loads(features).items()))): identity
            for identity, operation, implementation, scope, features in connection.execute(
                "SELECT id, operation, implementation, timing_scope, features FROM work_points"
            )
        }
        self.capture: int | None = None
        self.positions: dict[tuple[str, str], tuple[int, int, float]] = {}
        self.changed = False
        self.counts = dict(inserted=0, duplicate_imports=0)

    def select_capture(self, capture: int) -> None:
        if capture == self.capture:
            return

        self.flush()
        self.capture = capture
        for work, source, scope, distribution, positions in self.connection.execute(
            "SELECT o.work_id, o.source_id, w.timing_scope, o.distribution, o.positions "
            "FROM observations o JOIN work_points w ON w.id=o.work_id WHERE o.capture_id=?",
            (capture,),
        ):
            values = [duration for duration, _ in unpack_samples(distribution)]
            for position, index in unpack_samples(positions):
                self.positions[position, scope] = work, source, values[index]

    def add(
        self,
        capture: int,
        source: int,
        position: str,
        operation: str,
        implementation: str,
        scope: str,
        features: dict[str, float],
        duration: float,
    ) -> None:
        if not position or not operation or not scope or duration < 0 or not math.isfinite(duration):
            raise ValueError("cost samples need an operation, position, timing scope and finite nonnegative duration")
        duration = float(duration)
        self.select_capture(capture)
        # Numeric JSON spelling and signed zero do not define different work.
        key = operation, implementation, scope, tuple(sorted(features.items()))
        work = self.work_ids.get(key)
        if work is None:
            work = self.connection.execute(
                "INSERT INTO work_points (operation, implementation, timing_scope, features) VALUES (?, ?, ?, ?)",
                (
                    operation,
                    implementation,
                    scope,
                    canonical_json({name: float(value) if value else 0.0 for name, value in features.items()}),
                ),
            ).lastrowid
            self.work_ids[key] = work

        identity = position, scope
        stored = self.positions.get(identity)
        if stored is not None:
            if stored[0] != work or stored[2] != duration:
                raise ValueError(f"conflicting cost reimport: capture {capture}/{position}")
            self.counts["duplicate_imports"] += 1
            return

        self.positions[identity] = work, source, duration
        self.changed = True
        self.counts["inserted"] += 1

    def flush(self) -> None:
        if self.changed:
            grouped: dict[tuple[int, int], list[tuple[str, float]]] = defaultdict(list)
            for (position, _), (work, source, duration) in self.positions.items():
                grouped[work, source].append((position, duration))

            self.connection.execute("DELETE FROM observations WHERE capture_id=?", (self.capture,))
            for (work, source), rows in grouped.items():
                frequencies = Counter(duration for _, duration in rows)
                distribution = sorted(frequencies.items())
                indices = {duration: index for index, (duration, _) in enumerate(distribution)}
                self.connection.execute(
                    "INSERT INTO observations VALUES (?, ?, ?, ?, ?, ?)",
                    (
                        self.capture,
                        source,
                        work,
                        len(rows),
                        _pack(distribution),
                        _pack([(position, indices[duration]) for position, duration in sorted(rows)]),
                    ),
                )

        self.positions.clear()
        self.capture = None
        self.changed = False
