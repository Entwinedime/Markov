"""Profiling-suite expansion, selection, and contract summaries."""

from __future__ import annotations

import copy
from typing import Any

from ..common.naming import sanitize
from ..common.manifest import profile_labels


INTERNAL_SUITE_KEYS = {"experiments", "matrix", "continue_on_error", "$unset"}
MATRIX_ENTRY_META_KEYS = {"id", "name", "description", "$unset"}
EXPERIMENT_REF_KEYS = {"server_ref", "input_ref"}


def narrow_profile_channels(cfg: dict[str, Any], requested: set[str]) -> dict[str, Any]:
    """Return an auditable diagnostic config using a subset of suite channels.

    This override can only remove configured channels.  It changes the suite
    name and metadata so a Python-probe-only diagnostic can never be mistaken
    for the source suite's full-DAG profiling contract.
    """

    if not requested:
        return copy.deepcopy(cfg)
    canonical_requested = {value.strip().replace("-", "_").lower() for value in requested if value.strip()}
    if not canonical_requested:
        raise ValueError("diagnostic profiling channel override must not be empty")
    profiling = cfg.get("profiling")
    if not isinstance(profiling, dict):
        raise ValueError("diagnostic profiling channel override requires a profiling object")
    configured_raw = profiling.get("channels")
    if not isinstance(configured_raw, list) or not all(isinstance(value, str) for value in configured_raw):
        raise ValueError("diagnostic profiling channel override requires explicit configured channels")
    configured = [value.replace("-", "_").lower() for value in configured_raw]
    unknown = canonical_requested - set(configured)
    if unknown:
        raise ValueError(
            "diagnostic profiling channel override can only narrow configured channels; "
            f"unknown={sorted(unknown)} configured={configured}"
        )
    effective = [value for value in configured if value in canonical_requested]
    result = copy.deepcopy(cfg)
    result["name"] = f"{str(cfg.get('name') or 'profile_suite')}_diagnostic_{'_'.join(effective)}"
    result["profiling"]["channels"] = effective
    metadata = dict(result.get("metadata") or {})
    metadata["source_suite_name"] = cfg.get("name")
    metadata["source_suite_purpose"] = metadata.get("purpose")
    metadata["purpose"] = "Targeted low-overhead diagnostic capture; not a full-DAG modeling profile."
    metadata["profile_mode"] = "targeted_diagnostic_channel_subset"
    metadata["profiling_channel_override"] = {
        "configured": configured,
        "effective": effective,
        "narrowing_only": True,
    }
    metadata["full_dag_contract"] = (
        "NOT_APPLICABLE: this diagnostic deliberately omits channels and cannot be used as a DAG source."
    )
    metadata["execution_contract"] = (
        "Only explicitly selected cells run; planned=attempted=completed and zero failures remain required."
    )
    result["metadata"] = metadata
    return result


def deep_merge(base: dict[str, Any], override: dict[str, Any]) -> dict[str, Any]:
    """Recursively merge suite-common config with an experiment override."""

    merged = copy.deepcopy(base)
    for key, value in override.items():
        if key == "$unset":
            continue
        if key in merged and isinstance(merged[key], dict) and isinstance(value, dict):
            merged[key] = deep_merge(merged[key], value)
        else:
            merged[key] = copy.deepcopy(value)
    return merged


def delete_path(value: dict[str, Any], path: str) -> None:
    """Delete a dotted config path after common/experiment merging."""

    parts = [part for part in path.split(".") if part]
    if not parts:
        raise ValueError("$unset entries must not be empty")

    cursor: Any = value
    for part in parts[:-1]:
        if not isinstance(cursor, dict) or part not in cursor:
            return
        cursor = cursor[part]
    if isinstance(cursor, dict):
        cursor.pop(parts[-1], None)


def apply_unset(value: dict[str, Any], paths: Any) -> None:
    """Apply the suite ``$unset`` list to a merged configuration."""

    if paths is None:
        return
    if not isinstance(paths, list) or not all(isinstance(path, str) for path in paths):
        raise TypeError("$unset must be a list of dot-separated paths")
    for path in paths:
        delete_path(value, path)


def parse_experiment_selection(raw_values: list[str]) -> set[str]:
    """Parse repeated, comma-separated CLI selectors."""

    selected: set[str] = set()
    for raw in raw_values:
        for item in raw.split(","):
            item = item.strip()
            if item:
                selected.add(item)
    return selected


def experiment_identity(cfg: dict[str, Any], index: int) -> str:
    """Return a stable experiment identity with an ordinal fallback."""

    for key in ("id", "name"):
        value = cfg.get(key)
        if isinstance(value, str) and value.strip():
            return value.strip()
    return f"experiment-{index}"


def experiment_selectors(cfg: dict[str, Any], index: int) -> set[str]:
    """Return every stable CLI selector accepted for an experiment."""

    selectors = {str(index), f"{index:02d}"}
    for key in ("id", "name"):
        value = cfg.get(key)
        if isinstance(value, str) and value.strip():
            selectors.add(value.strip())
            selectors.add(sanitize(value))
    return selectors


def reject_profiling_override(value: dict[str, Any], context: str) -> None:
    """Reject local profiling overrides so one suite has one capture contract."""

    if "profiling" in value:
        raise ValueError(f"{context} must not override profiling; suite experiments share one profiling config")
    unset_paths = value.get("$unset")
    if isinstance(unset_paths, list):
        for path in unset_paths:
            if isinstance(path, str) and (path == "profiling" or path.startswith("profiling.")):
                raise ValueError(f"{context} must not unset profiling; suite experiments share one profiling config")


def matrix_entries(matrix: dict[str, Any], key: str) -> dict[str, dict[str, Any]]:
    """Read and validate either ``matrix.servers`` or ``matrix.inputs``."""

    raw_entries = matrix.get(key)
    if not isinstance(raw_entries, list) or not raw_entries:
        raise ValueError(f"matrix.{key} must be a non-empty list")

    entries: dict[str, dict[str, Any]] = {}
    for index, entry in enumerate(raw_entries):
        if not isinstance(entry, dict):
            raise TypeError(f"matrix.{key}[{index}] must be an object")
        reject_profiling_override(entry, f"matrix.{key}[{index}]")
        raw_id = entry.get("id")
        if not isinstance(raw_id, str) or not raw_id.strip():
            raise ValueError(f"matrix.{key}[{index}].id must be a non-empty string")
        entry_id = raw_id.strip()
        if entry_id in entries:
            raise ValueError(f"duplicate matrix.{key} id: {entry_id}")
        entries[entry_id] = entry
    return entries


def expand_suite(cfg: dict[str, Any]) -> list[dict[str, Any]]:
    """Expand a suite, returning a plain single-run config unchanged."""

    matrix = cfg.get("matrix")
    experiments = cfg.get("experiments")
    if experiments is None and matrix is None:
        return [cfg]

    if matrix is not None and not isinstance(matrix, dict):
        raise TypeError("matrix must be an object")
    if matrix is not None:
        servers = matrix_entries(matrix, "servers")
        inputs = matrix_entries(matrix, "inputs")
    if experiments is None:
        experiments = [{"server_ref": server, "input_ref": workload} for server in servers for workload in inputs]
    if not isinstance(experiments, list) or not experiments:
        raise ValueError("experiments must be a non-empty list")

    common = {key: value for key, value in cfg.items() if key not in INTERNAL_SUITE_KEYS}
    expanded = []
    for index, experiment in enumerate(experiments, start=1):
        if not isinstance(experiment, dict):
            raise TypeError(f"experiments[{index - 1}] must be an object")

        merged = common
        provenance = {}
        if matrix is not None:
            for axis, entries in (("server", servers), ("input", inputs)):
                reference = experiment.get(f"{axis}_ref")
                if not isinstance(reference, str) or not reference.strip():
                    raise ValueError(f"experiments[{index - 1}].{axis}_ref must reference matrix.{axis}s")
                entry_id = reference.strip()
                if entry_id not in entries:
                    raise ValueError(f"experiments[{index - 1}].{axis}_ref references unknown {axis}: {entry_id}")

                entry = entries[entry_id]
                merged = deep_merge(
                    merged, {key: value for key, value in entry.items() if key not in MATRIX_ENTRY_META_KEYS}
                )
                apply_unset(merged, entry.get("$unset"))
                provenance[f"suite_{axis}_id"] = entry_id

            default_id = f"{provenance['suite_server_id']}_{provenance['suite_input_id']}"
            experiment_id = str(experiment.get("id") or default_id).strip()
            if not experiment_id:
                raise ValueError(f"experiments[{index - 1}].id must not be empty")
            override = {key: value for key, value in experiment.items() if key not in EXPERIMENT_REF_KEYS}
        else:
            experiment_id = experiment_identity(experiment, index)
            override = experiment

        reject_profiling_override(override, f"experiments[{index - 1}]")
        merged = deep_merge(merged, override)
        apply_unset(merged, experiment.get("$unset"))
        merged["id"] = experiment_id
        merged["name"] = str(experiment.get("name") or experiment_id)

        metadata = merged.get("metadata")
        if metadata is None:
            metadata = {}
        if not isinstance(metadata, dict):
            raise TypeError("metadata must be an object")
        metadata.setdefault("suite_experiment_id", experiment_id)
        for key, value in provenance.items():
            metadata.setdefault(key, value)
        merged["metadata"] = metadata
        expanded.append(merged)
    return expanded


def filter_suite_experiments(
    experiments: list[tuple[int, dict[str, Any]]],
    selected_experiments: set[str],
    *,
    selected_inputs: set[str] | None = None,
    selected_servers: set[str] | None = None,
) -> list[tuple[int, dict[str, Any]]]:
    """Filter expanded experiments by CLI selectors without changing their order."""

    if not selected_experiments:
        selected = list(experiments)
    else:
        selected = []
        matched: set[str] = set()
        for index, experiment in experiments:
            selectors = experiment_selectors(experiment, index)
            overlap = selected_experiments & selectors
            if overlap:
                selected.append((index, experiment))
                matched.update(overlap)

        missing = sorted(selected_experiments - matched)
        if missing:
            available = ", ".join(str(item[1].get("id") or item[1].get("name") or item[0]) for item in experiments)
            raise ValueError(f"unknown experiment selector(s): {', '.join(missing)}; available: {available}")

    for position, axis, requested in ((1, "input", selected_inputs), (0, "server", selected_servers)):
        if not requested:
            continue
        available = {profile_labels(experiment)[position] for _, experiment in experiments} - {""}
        missing = requested - available
        if missing:
            raise ValueError(
                f"unknown {axis} selector(s): {', '.join(sorted(missing))}; "
                f"available {axis}s: {', '.join(sorted(available))}"
            )
        selected = [
            (index, experiment) for index, experiment in selected if profile_labels(experiment)[position] in requested
        ]
    if not selected:
        raise ValueError("no experiments matched the selected experiment/input/server combination")
    return selected


def suite_profile_mode(cfg: dict[str, Any]) -> str | None:
    """Return the optional profile mode declared by suite metadata."""

    metadata = cfg.get("metadata") if isinstance(cfg.get("metadata"), dict) else {}
    value = metadata.get("profile_mode")
    return str(value) if isinstance(value, str) and value else None
