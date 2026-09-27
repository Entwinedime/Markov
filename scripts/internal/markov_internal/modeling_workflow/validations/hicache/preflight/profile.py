"""Readiness of source HiCache facts consumed by the C++ model."""

from __future__ import annotations

from pathlib import Path
from typing import Any

from markov_internal.common.io import load_json
from markov_internal.common.manifest import manifest_files
from markov_internal.common.paths import map_repo_path
from markov_internal.common.trace import load_chrome_trace_events

from ..core.facts import HICACHE_CONSUMER_STATE_MODEL
from .state_fact_accumulator import HiCacheStateFactAccumulator


def audit_hicache_profile(manifest_path: Path) -> dict[str, Any]:
    """Check only artifacts and state facts required by source replay."""

    manifest = load_json(manifest_path)
    if not isinstance(manifest, dict):
        raise ValueError(f"profile manifest must be a JSON object: {manifest_path}")
    profiling = manifest.get("profiling") if isinstance(manifest.get("profiling"), dict) else {}
    sidecar = manifest.get("sidecar") if isinstance(manifest.get("sidecar"), dict) else {}
    trace = manifest.get("trace") if isinstance(manifest.get("trace"), dict) else {}
    requested_consumers = {
        str(consumer) for consumer in profiling.get("python_consumers") or [] if isinstance(consumer, str)
    }
    state_model_enabled = HICACHE_CONSUMER_STATE_MODEL in requested_consumers
    files = {
        "torch": manifest_files(trace.get("torch_trace_files", [])),
        "ld_preload": manifest_files(trace.get("ld_preload_trace_files", [])),
        "python_probe": manifest_files(sidecar.get("python_probe_files", [])),
    }
    for paths in files.values():
        for path in paths:
            if not path.is_file():
                raise FileNotFoundError(f"manifest-declared trace file is unavailable: {path}")

    channels = {value for value in profiling.get("channels_enabled") or [] if isinstance(value, str)}
    missing_channels = sorted(channel for channel in channels if not files.get(channel))
    contract = profiling.get("python_target_contract")
    selected_targets = (
        {value for value in contract["selected_target_ids"] if isinstance(value, str)}
        if isinstance(contract, dict) and isinstance(contract.get("selected_target_ids"), list)
        else set()
    )

    # Keep only coverage state between files, not a second combined event list.
    accumulator = HiCacheStateFactAccumulator()
    observed_targets: set[str] = set()
    event_count = 0
    probe_failed = False
    for path in files["python_probe"]:
        for event in load_chrome_trace_events(path):
            args = event.get("args") if isinstance(event.get("args"), dict) else {}
            if not (
                event.get("cat") == "python_probe"
                or args.get("domain") == "python_probe"
                or str(event.get("name") or "").startswith("hicache_")
            ):
                continue
            event_count += 1
            if args.get("target_id"):
                observed_targets.add(str(args["target_id"]))
            probe_failed |= args.get("phase") == "exception" or args.get("status") == "exception"
            accumulator.observe(args)

    artifact_errors = []
    if missing_channels:
        artifact_errors.append("trace_channel_missing")
    if (
        {"torch", "ld_preload", "python_probe"} <= channels
        and files["python_probe"]
        and not (files["torch"] or files["ld_preload"])
    ):
        artifact_errors.append("sidecar_only_trace")
    if "python_probe" in channels and not files["python_probe"]:
        artifact_errors.append("missing_python_probe_files")
    if selected_targets and not (selected_targets & observed_targets):
        artifact_errors.append("all_python_probe_targets_missing")
    if probe_failed:
        artifact_errors.append("python_probe_exception_events")
    artifact_errors.sort()

    coverage = accumulator.finalize()
    state_errors = state_fact_errors(coverage, enabled=state_model_enabled)
    blocking_artifact_errors = sorted(
        error
        for error in artifact_errors
        if error in {"missing_python_probe_files", "all_python_probe_targets_missing", "python_probe_exception_events"}
    )
    workflow_errors = sorted(set(blocking_artifact_errors + state_errors))
    return {
        "manifest_path": str(manifest_path),
        "run_dir": str(map_repo_path(Path(str(manifest.get("run_dir") or manifest_path.parent)))),
        "profiling_ready": bool(manifest.get("profiling_ready")),
        "status": manifest.get("status"),
        "trace_files": {channel: len(paths) for channel, paths in files.items()},
        "trace_channel_coverage": {
            **{f"{channel}_trace_files": len(paths) for channel, paths in files.items()},
            "channels_enabled": sorted(channels),
        },
        "missing_trace_channels": missing_channels,
        "python_probe_events": event_count,
        "configured_target_count": len(selected_targets),
        "observed_target_count": len(selected_targets & observed_targets),
        "artifact_errors": artifact_errors,
        "artifact_ready": not artifact_errors,
        "requested_consumers": sorted(requested_consumers),
        "hicache_state_model_enabled": state_model_enabled,
        "hicache_state_model_fact_coverage": coverage,
        "workflow_input_errors": workflow_errors,
        "workflow_input_ready": not workflow_errors,
    }


def state_fact_errors(coverage: dict[str, Any], *, enabled: bool) -> list[str]:
    if not enabled:
        return ["hicache_state_model_consumer_not_enabled"]
    errors: list[str] = []
    if coverage["missing_required_fact_events"] > 0:
        errors.append("hicache_state_model_facts_missing")
    if coverage["missing_token_dictionary_refs"] or coverage["dictionary_ids_without_tokens"]:
        errors.append("hicache_token_dictionary_missing")
    if coverage["invalid_token_dictionary_issue_count"] > 0:
        errors.append("hicache_token_dictionary_invalid")
    if coverage["seq_order_error_count"] > 0:
        errors.append("hicache_state_fact_seq_invalid")
    return sorted(set(errors))
