"""Minimal file/channel readiness shared by profiling consumers."""

from __future__ import annotations

from pathlib import Path
from typing import Any

from ..common.io import load_json
from ..common.manifest import existing_manifest_files
from ..common.paths import map_repo_path
from ..common.trace import load_chrome_trace_events


def audit_profile_artifacts(
    manifest_path: Path,
    *,
    python_probe_events: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    """Check declared trace files and lightweight Python-probe health."""

    manifest = load_json(manifest_path)
    if not isinstance(manifest, dict):
        raise ValueError(f"profile manifest must be a JSON object: {manifest_path}")
    profiling = manifest.get("profiling") if isinstance(manifest.get("profiling"), dict) else {}
    trace = manifest.get("trace") if isinstance(manifest.get("trace"), dict) else {}
    sidecar = manifest.get("sidecar") if isinstance(manifest.get("sidecar"), dict) else {}
    channels = {str(value) for value in profiling.get("channels_enabled") or [] if isinstance(value, str)}
    files = {
        "torch": _torch_files(trace),
        "ld_preload": existing_manifest_files(trace.get("ld_preload_trace_files", [])),
        "python_probe": existing_manifest_files(sidecar.get("python_probe_files", [])),
    }
    coverage = {f"{channel}_trace_files": len(paths) for channel, paths in files.items()}
    coverage["channels_enabled"] = sorted(channels)
    missing_channels = sorted(channel for channel in channels if not files.get(channel))
    events = python_probe_events if python_probe_events is not None else load_python_probe_events(files["python_probe"])
    selected_targets = _selected_targets(profiling)
    observed_targets = {
        str(args.get("target_id"))
        for event in events
        if isinstance((args := event.get("args")), dict) and args.get("target_id")
    }
    errors: list[str] = []
    if missing_channels:
        errors.append("trace_channel_missing")
    if (
        {"torch", "ld_preload", "python_probe"}.issubset(channels)
        and files["python_probe"]
        and not (files["torch"] or files["ld_preload"])
    ):
        errors.append("sidecar_only_trace")
    if "python_probe" in channels and not files["python_probe"]:
        errors.append("missing_python_probe_files")
    if selected_targets and not (selected_targets & observed_targets):
        errors.append("all_python_probe_targets_missing")
    if any(_probe_failed(event) for event in events):
        errors.append("python_probe_exception_events")
    return {
        "manifest_path": str(manifest_path),
        "run_dir": str(map_repo_path(Path(str(manifest.get("run_dir") or manifest_path.parent)))),
        "profiling_ready": bool(manifest.get("profiling_ready")),
        "status": manifest.get("status"),
        "trace_files": {channel: len(paths) for channel, paths in files.items()},
        "trace_channel_coverage": coverage,
        "missing_trace_channels": missing_channels,
        "python_probe_events": len(events),
        "configured_target_count": len(selected_targets),
        "observed_target_count": len(selected_targets & observed_targets),
        "artifact_errors": sorted(set(errors)),
        "artifact_ready": not errors,
    }


def load_python_probe_events(paths: list[Path]) -> list[dict[str, Any]]:
    events: list[dict[str, Any]] = []
    for path in paths:
        rows, _status = load_chrome_trace_events(path, auto_repair=True)
        events.extend(event for event in rows if is_python_probe_trace_event(event))
    return events


def is_python_probe_trace_event(event: dict[str, Any]) -> bool:
    args = event.get("args") if isinstance(event.get("args"), dict) else {}
    return (
        event.get("cat") == "python_probe"
        or args.get("domain") == "python_probe"
        or str(event.get("name") or "").startswith("hicache_")
    )


def _torch_files(trace: dict[str, Any]) -> list[Path]:
    files = existing_manifest_files(trace.get("torch_trace_files", []))
    raw_dir = trace.get("torch_trace_dir")
    if files or not isinstance(raw_dir, str):
        return files
    directory = map_repo_path(Path(raw_dir))
    return sorted(directory.glob("**/trace_view.json")) if directory.is_dir() else []


def _selected_targets(profiling: dict[str, Any]) -> set[str]:
    contract = profiling.get("python_target_contract")
    if not isinstance(contract, dict) or not isinstance(contract.get("selected_target_ids"), list):
        return set()
    return {str(value) for value in contract["selected_target_ids"] if isinstance(value, str)}


def _probe_failed(event: dict[str, Any]) -> bool:
    args = event.get("args") if isinstance(event.get("args"), dict) else {}
    return args.get("phase") == "exception" or args.get("status") == "exception"
