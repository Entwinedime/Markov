"""Profiling-suite expansion, selection, and contract summaries."""

from __future__ import annotations

import copy
from typing import Any

from ..common.naming import sanitize


INTERNAL_SUITE_KEYS = {"experiments", "matrix", "continue_on_error", "$unset"}
MATRIX_ENTRY_META_KEYS = {"id", "name", "description", "$unset"}
EXPERIMENT_REF_KEYS = {"server_ref", "input_ref"}
PROFILE_EXPERIMENTS_ENV = "TRACE_SIM_PROFILE_EXPERIMENTS"
PROFILE_INPUTS_ENV = "TRACE_SIM_PROFILE_INPUTS"
PROFILE_SERVERS_ENV = "TRACE_SIM_PROFILE_SERVERS"
PROFILE_FORCED_TOKEN_BUNDLE_ENV = "TRACE_SIM_FORCED_TOKEN_BUNDLE"


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


def parse_experiment_selection(raw_values: list[str] | None, env_value: str | None = None) -> set[str]:
    """Parse comma-separated selectors from CLI values and an environment value."""

    selected: set[str] = set()
    for raw in [*(raw_values or []), env_value or ""]:
        for item in str(raw).split(","):
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


def describe_suite_experiment(index: int, cfg: dict[str, Any]) -> dict[str, Any]:
    """Build the experiment descriptor used by listing and selection artifacts."""

    metadata = cfg.get("metadata") if isinstance(cfg.get("metadata"), dict) else {}
    return {
        "index": index,
        "id": cfg.get("id", experiment_identity(cfg, index)),
        "name": cfg.get("name", experiment_identity(cfg, index)),
        "server_id": metadata.get("suite_server_id"),
        "input_id": metadata.get("suite_input_id"),
        "profile_mode": metadata.get("profile_mode"),
        "selectors": sorted(experiment_selectors(cfg, index)),
    }


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


def matrix_entry_override(entry: dict[str, Any]) -> dict[str, Any]:
    """Strip matrix metadata while preserving fields that participate in merge."""

    return {key: value for key, value in entry.items() if key not in MATRIX_ENTRY_META_KEYS}


def attach_suite_metadata(
    cfg: dict[str, Any],
    *,
    experiment_id: str,
    server_id: str | None,
    input_id: str | None,
) -> None:
    """Attach suite provenance to one expanded experiment."""

    metadata = cfg.get("metadata")
    if metadata is None:
        metadata = {}
    if not isinstance(metadata, dict):
        raise TypeError("metadata must be an object")
    metadata = dict(metadata)
    metadata.setdefault("suite_experiment_id", experiment_id)
    if server_id is not None:
        metadata.setdefault("suite_server_id", server_id)
    if input_id is not None:
        metadata.setdefault("suite_input_id", input_id)
    cfg["metadata"] = metadata


def generated_matrix_experiments(matrix: dict[str, Any]) -> list[dict[str, Any]]:
    """Generate the server/input Cartesian product when experiments are omitted."""

    servers = matrix_entries(matrix, "servers")
    inputs = matrix_entries(matrix, "inputs")
    experiments: list[dict[str, Any]] = []
    for server_id in servers:
        for input_id in inputs:
            experiment_id = f"{server_id}_{input_id}"
            experiments.append(
                {
                    "id": experiment_id,
                    "name": experiment_id,
                    "server_ref": server_id,
                    "input_ref": input_id,
                }
            )
    return experiments


def expand_matrix_experiment(
    common: dict[str, Any],
    matrix: dict[str, Any],
    experiment: dict[str, Any],
    index: int,
) -> dict[str, Any]:
    """Expand one matrix experiment into an executable single-run config."""

    servers = matrix_entries(matrix, "servers")
    inputs = matrix_entries(matrix, "inputs")

    server_ref = experiment.get("server_ref")
    input_ref = experiment.get("input_ref")
    if not isinstance(server_ref, str) or not server_ref.strip():
        raise ValueError(f"experiments[{index - 1}].server_ref must reference matrix.servers")
    if not isinstance(input_ref, str) or not input_ref.strip():
        raise ValueError(f"experiments[{index - 1}].input_ref must reference matrix.inputs")
    server_id = server_ref.strip()
    input_id = input_ref.strip()
    if server_id not in servers:
        raise ValueError(f"experiments[{index - 1}].server_ref references unknown server: {server_id}")
    if input_id not in inputs:
        raise ValueError(f"experiments[{index - 1}].input_ref references unknown input: {input_id}")

    experiment_id = str(experiment.get("id") or f"{server_id}_{input_id}").strip()
    if not experiment_id:
        raise ValueError(f"experiments[{index - 1}].id must not be empty")
    merged = deep_merge(common, matrix_entry_override(servers[server_id]))
    apply_unset(merged, servers[server_id].get("$unset"))
    merged = deep_merge(merged, matrix_entry_override(inputs[input_id]))
    apply_unset(merged, inputs[input_id].get("$unset"))

    experiment_override = {key: value for key, value in experiment.items() if key not in EXPERIMENT_REF_KEYS}
    reject_profiling_override(experiment_override, f"experiments[{index - 1}]")
    merged = deep_merge(merged, experiment_override)
    apply_unset(merged, experiment.get("$unset"))
    merged["id"] = experiment_id
    merged["name"] = str(experiment.get("name") or experiment_id)
    attach_suite_metadata(
        merged,
        experiment_id=experiment_id,
        server_id=server_id,
        input_id=input_id,
    )
    return merged


def expand_suite(cfg: dict[str, Any]) -> list[dict[str, Any]]:
    """Expand a suite, returning a plain single-run config unchanged."""

    matrix = cfg.get("matrix")
    experiments = cfg.get("experiments")
    if experiments is None and matrix is None:
        return [cfg]

    if matrix is not None and not isinstance(matrix, dict):
        raise TypeError("matrix must be an object")
    if experiments is None:
        experiments = generated_matrix_experiments(matrix)
    if not isinstance(experiments, list) or not experiments:
        raise ValueError("experiments must be a non-empty list")

    common = {key: value for key, value in cfg.items() if key not in INTERNAL_SUITE_KEYS}
    expanded = []
    for index, experiment in enumerate(experiments, start=1):
        if not isinstance(experiment, dict):
            raise TypeError(f"experiments[{index - 1}] must be an object")
        if matrix is not None:
            merged = expand_matrix_experiment(common, matrix, experiment, index)
        else:
            reject_profiling_override(experiment, f"experiments[{index - 1}]")
            experiment_id = experiment_identity(experiment, index)
            merged = deep_merge(common, experiment)
            apply_unset(merged, experiment.get("$unset"))
            merged["id"] = experiment_id
            merged["name"] = str(experiment.get("name") or experiment_id)
            attach_suite_metadata(
                merged,
                experiment_id=experiment_id,
                server_id=None,
                input_id=None,
            )
        expanded.append(merged)
    return expanded


def filter_suite_experiments(
    experiments: list[tuple[int, dict[str, Any]]],
    selected_experiments: set[str],
    *,
    selected_inputs: set[str] | None = None,
    selected_servers: set[str] | None = None,
) -> list[tuple[int, dict[str, Any]]]:
    """Filter expanded experiments by validated CLI/environment selectors."""

    selected_inputs = selected_inputs or set()
    selected_servers = selected_servers or set()
    available_inputs = sorted(
        {
            str((experiment.get("metadata") or {}).get("suite_input_id"))
            for _index, experiment in experiments
            if isinstance(experiment.get("metadata"), dict) and (experiment.get("metadata") or {}).get("suite_input_id")
        }
    )
    available_servers = sorted(
        {
            str((experiment.get("metadata") or {}).get("suite_server_id"))
            for _index, experiment in experiments
            if isinstance(experiment.get("metadata"), dict)
            and (experiment.get("metadata") or {}).get("suite_server_id")
        }
    )

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

    missing_inputs = selected_inputs - set(available_inputs)
    if missing_inputs:
        raise ValueError(
            f"unknown input selector(s): {', '.join(sorted(missing_inputs))}; "
            f"available inputs: {', '.join(available_inputs)}"
        )
    missing_servers = selected_servers - set(available_servers)
    if missing_servers:
        raise ValueError(
            f"unknown server selector(s): {', '.join(sorted(missing_servers))}; "
            f"available servers: {', '.join(available_servers)}"
        )

    def metadata_value(experiment: dict[str, Any], key: str) -> str:
        """Read one string-valued suite metadata field."""

        metadata = experiment.get("metadata") if isinstance(experiment.get("metadata"), dict) else {}
        value = metadata.get(key)
        return str(value) if isinstance(value, str) else ""

    filtered = [
        (index, experiment)
        for index, experiment in selected
        if (not selected_inputs or metadata_value(experiment, "suite_input_id") in selected_inputs)
        and (not selected_servers or metadata_value(experiment, "suite_server_id") in selected_servers)
    ]
    if not filtered:
        raise ValueError("no experiments matched the selected experiment/input/server combination")
    return filtered


def suite_profile_mode(cfg: dict[str, Any]) -> str | None:
    """Return the optional profile mode declared by suite metadata."""

    metadata = cfg.get("metadata") if isinstance(cfg.get("metadata"), dict) else {}
    value = metadata.get("profile_mode")
    return str(value) if isinstance(value, str) and value else None


def summarize_suite_forced_token_contracts(contracts: list[dict[str, Any]]) -> dict[str, Any]:
    """Summarize forced-token preflight identities across a suite."""

    modes = sorted({str(contract.get("mode") or "none") for contract in contracts})
    errors = sorted({str(error) for contract in contracts for error in contract.get("errors", [])})
    plan_hashes = sorted(
        {
            str(plan.get("sha256"))
            for contract in contracts
            for plan in [contract.get("plan")]
            if isinstance(plan, dict) and plan.get("sha256")
        }
    )
    bundle_hashes = sorted(
        {
            str(bundle.get("sha256"))
            for contract in contracts
            for bundle in [contract.get("bundle")]
            if isinstance(bundle, dict) and bundle.get("sha256")
        }
    )
    bundle_ids = sorted(
        {
            str(bundle.get("bundle_id"))
            for contract in contracts
            for bundle in [contract.get("bundle")]
            if isinstance(bundle, dict) and bundle.get("bundle_id")
        }
    )
    workloads = sorted({str(contract.get("workload_id")) for contract in contracts if contract.get("workload_id")})
    return {
        "mode_count": {
            mode: sum(1 for contract in contracts if str(contract.get("mode") or "none") == mode) for mode in modes
        },
        "errors": errors,
        "ready": not errors,
        "workload_ids": workloads,
        "plan_sha256_count": len(plan_hashes),
        "plan_sha256_values": plan_hashes,
        "bundle_sha256_count": len(bundle_hashes),
        "bundle_sha256_values": bundle_hashes,
        "bundle_ids": bundle_ids,
    }
