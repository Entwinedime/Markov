"""Parse independent control measurement inputs for explicit database import."""

from __future__ import annotations

from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Any

from ...common.io import load_json
from ...common.paths import require_repo_path, repo_relative_path

if TYPE_CHECKING:
    from ..source_dag.profiles import ProfileRunRef


_SHARED_OPERATIONS = (
    "release_host",
    "write_confirmation",
    "prefetch_query",
    "load_index",
    "load_submission",
    "layer_wait",
)
CALIBRATION_FIELDS = tuple(f"{kind}_calibration" for kind in ("prefetch_wait", "write_host", *_SHARED_OPERATIONS)) + (
    "prefetch_cpu_calibrations",
)


@dataclass(frozen=True)
class ControlMeasurement:
    """One declared control document; selection retains its policy/page keys.

    The group owns the loaded document and parsed source. Consumers treat
    them as read-only; CPU pair provenance is resolved at the import boundary.
    """

    selection: tuple[str, ...]
    path: Path
    document: dict[str, Any]
    source: ProfileRunRef


def control_entries(collection: dict[str, Any]) -> Iterator[tuple[tuple[str, ...], Any]]:
    """Walk declared paths or embedded documents without opening any files."""

    def entries(value: Any, selection: tuple[str, ...]) -> Iterator[tuple[tuple[str, ...], Any]]:
        if isinstance(value, str) or "role" in value:
            yield selection, value
            return

        for key, child in value.items():
            yield from entries(child, (*selection, key))

    for key, value in collection.items():
        yield from entries(value, (key,))


def read_control_measurements(collection: dict[str, Any]) -> tuple[ControlMeasurement, ...]:
    """Read documents and their source identities once, retaining selection keys."""

    # Profile parsing belongs to this input boundary. Pure control traversal
    # and stored database provenance do not load profiles or measurement files.
    from ..source_dag.profiles import discover_profile_runs

    documents: dict[Path, dict[str, Any]] = {}
    sources: dict[Path, ProfileRunRef] = {}
    measurements = []
    for selection, value in control_entries(collection):
        path = require_repo_path(value)
        if path not in documents:
            document = load_json(path)
            if "role" not in document:
                raise ValueError(f"control calibration requires an exported measurement document: {path}")
            documents[path] = document

        document = documents[path]
        manifest = require_repo_path(document["source_manifest"]).resolve()
        if manifest not in sources:
            (source,) = discover_profile_runs((manifest,))
            sources[manifest] = source

        measurements.append(ControlMeasurement(selection, path, document, sources[manifest]))

    return tuple(sorted(measurements, key=lambda measurement: str(measurement.path)))


def control_calibrations(*collections: dict[str, Any]) -> dict[str, Any]:
    """Parse and merge declarations; later entries override the same operation.

    Paths are repository-relative. Inputs are not mutated; operation semantics
    remain the C++ consumer's responsibility.
    """

    def path(value: str) -> str:
        if not isinstance(value, str) or not value:
            raise ValueError("control calibration requires a nonempty file path")
        return str(repo_relative_path(require_repo_path(value)))

    result: dict[str, Any] = {"prefetch_wait": {}, "write_host": {}}
    for raw in collections:
        if not isinstance(raw, dict) or set(raw) - {"prefetch_wait", "write_host", *_SHARED_OPERATIONS}:
            raise ValueError("invalid shared control_calibrations fields")
        for policy, value in raw.get("prefetch_wait", {}).items():
            if policy not in {"best_effort", "timeout", "wait_complete"}:
                raise ValueError(f"unknown calibration prefetch policy: {policy}")
            result["prefetch_wait"][policy] = path(value)
        for page, policies in raw.get("write_host", {}).items():
            if not isinstance(page, str) or not page.isdecimal() or int(page) <= 0 or str(int(page)) != page:
                raise ValueError("write calibration page size must be a positive decimal key")
            writes = result["write_host"].setdefault(page, {})
            for policy, value in policies.items():
                if policy not in {"write_back", "write_through", "write_through_selective"}:
                    raise ValueError(f"unknown calibration write policy: {policy}")
                writes[policy] = path(value)
        for kind in _SHARED_OPERATIONS:
            if kind in raw:
                result[kind] = path(raw[kind])
    return result
