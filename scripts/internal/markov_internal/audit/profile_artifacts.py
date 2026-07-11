#!/usr/bin/env python3
"""Generic post-profile artifact audit.

This module checks files, trace channels, and configured Python probe targets
left by a profiling run. It does not decide whether a backend model can consume
the run; consumer-specific readiness belongs to domain validation modules under
``markov_internal.modeling_workflow.validations``.
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from ..common.manifest import existing_manifest_files
from ..common.paths import ROOT_DIR, map_repo_path
from ..common.trace import load_chrome_trace_events


@dataclass
class TargetArtifactAudit:
    """Coverage and required-field summary for one configured probe target."""

    target_id: str
    configured: bool = True
    target: str = ""
    events_total: int = 0
    phases: Counter[str] = field(default_factory=Counter)
    statuses: Counter[str] = field(default_factory=Counter)
    missing_required_fields: Counter[str] = field(default_factory=Counter)
    field_presence: Counter[str] = field(default_factory=Counter)
    request_ids: set[str] = field(default_factory=set)
    operation_ids: set[str] = field(default_factory=set)
    node_ids: set[str] = field(default_factory=set)

    def observe(self, args: dict[str, Any]) -> None:
        """Accumulate coverage from one Python probe event payload."""

        self.events_total += 1
        self.phases[str(args.get("phase") or "unknown")] += 1
        self.statuses[str(args.get("status") or "unknown")] += 1
        for field_name in args.get("missing_required_fields") or []:
            self.missing_required_fields[str(field_name)] += 1
        for field_name, value in args.items():
            if value is not None and field_name not in {"missing_required_fields"}:
                self.field_presence[field_name] += 1
        _add_optional(self.request_ids, args.get("request_id"))
        _add_optional(self.operation_ids, args.get("operation_id"))
        _add_optional(self.node_ids, args.get("node_id"))
        _add_optional(self.node_ids, args.get("last_host_node_id"))
        _add_optional(self.node_ids, args.get("last_device_node_id"))
        _add_optional(self.node_ids, args.get("best_match_node_id"))

    def to_dict(self) -> dict[str, Any]:
        """Serialize counters and identity sets into deterministic JSON values."""

        return {
            "configured": self.configured,
            "target": self.target,
            "events_total": self.events_total,
            "phases": dict(sorted(self.phases.items())),
            "statuses": dict(sorted(self.statuses.items())),
            "missing_required_fields": dict(sorted(self.missing_required_fields.items())),
            "field_presence": dict(sorted(self.field_presence.items())),
            "request_id_count": len(self.request_ids),
            "operation_id_count": len(self.operation_ids),
            "node_id_count": len(self.node_ids),
        }


def main(argv: list[str] | None = None) -> int:
    """Run the generic profile artifact audit CLI."""

    args = parse_args(argv)
    manifest_path = resolve_path(args.manifest)
    result = audit_profile_artifacts(manifest_path)
    output_path = resolve_output_path(args.output, manifest_path, result)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    sys.stdout.write(str(output_path) + "\n")
    return 0 if result.get("artifact_ready") else 1


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """Parse generic profile artifact audit arguments."""

    parser = argparse.ArgumentParser(description="Audit generic profiling artifacts.")
    parser.add_argument("--manifest", required=True, help="profile_manifest.json path")
    parser.add_argument(
        "--output",
        help="output JSON path; defaults to run_dir/profile_artifact_audit.json",
    )
    return parser.parse_args(argv)


def audit_profile_artifacts(
    manifest_path: Path,
    *,
    python_probe_events: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    """Audit files, trace channels, and configured Python probe targets.

    Callers that already parsed the Python sidecar may pass its filtered events
    to avoid another full JSON decode during workflow preflight.
    """

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if not isinstance(manifest, dict):
        raise ValueError(f"profile manifest must be a JSON object: {manifest_path}")
    run_dir = map_repo_path(Path(str(manifest.get("run_dir") or manifest_path.parent)))
    profiling = manifest.get("profiling") if isinstance(manifest.get("profiling"), dict) else {}
    trace = manifest.get("trace") if isinstance(manifest.get("trace"), dict) else {}
    sidecar = manifest.get("sidecar") if isinstance(manifest.get("sidecar"), dict) else {}

    configured = configured_targets(profiling)
    channels_enabled = {str(channel) for channel in profiling.get("channels_enabled") or [] if isinstance(channel, str)}
    python_channel_enabled = "python_probe" in channels_enabled or bool(configured)
    python_probe_files = existing_manifest_files(sidecar.get("python_probe_files", []))
    events = python_probe_events if python_probe_events is not None else load_python_probe_events(python_probe_files)
    target_audits, unknown_targets = _audit_python_probe_targets(configured, events)

    missing_targets = sorted(target_id for target_id, audit in target_audits.items() if audit.events_total == 0)
    targets_with_missing_fields = sorted(
        target_id for target_id, audit in target_audits.items() if audit.missing_required_fields
    )
    exception_targets = sorted(
        target_id
        for target_id, audit in {**target_audits, **unknown_targets}.items()
        if audit.phases.get("exception", 0) > 0 or audit.statuses.get("exception", 0) > 0
    )

    torch_files = existing_manifest_files(trace.get("torch_trace_files", []))
    if not torch_files:
        torch_files = existing_dir_files(trace.get("torch_trace_dir"), "**/trace_view.json")
    ld_preload_files = existing_manifest_files(trace.get("ld_preload_trace_files", []))
    trace_channel_coverage = {
        "torch_trace_files": len(torch_files),
        "ld_preload_trace_files": len(ld_preload_files),
        "python_probe_trace_files": len(python_probe_files),
        "channels_enabled": sorted(channels_enabled),
    }
    missing_channels = trace_channel_missing_channels(channels_enabled, trace_channel_coverage)

    artifact_errors = _artifact_errors(
        missing_channels=missing_channels,
        channels_enabled=channels_enabled,
        python_channel_enabled=python_channel_enabled,
        python_probe_files=python_probe_files,
        torch_files=torch_files,
        ld_preload_files=ld_preload_files,
        configured_target_count=len(configured),
        missing_target_count=len(missing_targets),
        targets_with_missing_fields=targets_with_missing_fields,
        exception_targets=exception_targets,
    )

    return {
        "schema": "trace_sim.profile_artifact_audit.v1",
        "manifest_path": str(manifest_path),
        "run_dir": str(run_dir),
        "profiling_ready": bool(manifest.get("profiling_ready")),
        "status": manifest.get("status"),
        "dry_run": bool(manifest.get("dry_run")),
        "trace_files": {
            "torch": len(torch_files),
            "ld_preload": len(ld_preload_files),
            "python_probe": len(python_probe_files),
        },
        "trace_channel_coverage": trace_channel_coverage,
        "missing_trace_channels": missing_channels,
        "python_probe_events": len(events),
        "configured_target_count": len(configured),
        "observed_target_count": sum(1 for audit in target_audits.values() if audit.events_total > 0),
        "missing_targets": missing_targets,
        "targets_with_missing_required_fields": targets_with_missing_fields,
        "exception_targets": exception_targets,
        "unknown_targets": sorted(unknown_targets),
        "targets": {target_id: audit.to_dict() for target_id, audit in sorted(target_audits.items())},
        "unknown_target_details": {target_id: audit.to_dict() for target_id, audit in sorted(unknown_targets.items())},
        "artifact_errors": artifact_errors,
        "artifact_ready": not artifact_errors,
    }


def _audit_python_probe_targets(
    configured: dict[str, dict[str, Any]],
    events: list[dict[str, Any]],
) -> tuple[dict[str, TargetArtifactAudit], dict[str, TargetArtifactAudit]]:
    """Aggregate configured and unexpected Python probe target coverage."""

    target_audits = {
        target_id: TargetArtifactAudit(
            target_id=target_id,
            configured=True,
            target=str(target.get("target") or ""),
        )
        for target_id, target in configured.items()
    }
    unknown_targets: dict[str, TargetArtifactAudit] = {}
    for event in events:
        args = event.get("args")
        if not isinstance(args, dict):
            continue
        target_id = str(args.get("target_id") or "unknown")
        target = target_audits.get(target_id)
        if target is None:
            target = unknown_targets.setdefault(
                target_id,
                TargetArtifactAudit(target_id=target_id, configured=False),
            )
        target.observe(args)
    return target_audits, unknown_targets


def _artifact_errors(
    *,
    missing_channels: list[str],
    channels_enabled: set[str],
    python_channel_enabled: bool,
    python_probe_files: list[Path],
    torch_files: list[Path],
    ld_preload_files: list[Path],
    configured_target_count: int,
    missing_target_count: int,
    targets_with_missing_fields: list[str],
    exception_targets: list[str],
) -> list[str]:
    """Apply generic artifact readiness rules in stable diagnostic order."""

    errors: list[str] = []
    if missing_channels:
        errors.append("trace_channel_missing")
    if full_dag_channels_enabled(channels_enabled) and python_probe_files and not torch_files and not ld_preload_files:
        errors.append("sidecar_only_trace")
    if python_channel_enabled and not python_probe_files:
        errors.append("missing_python_probe_files")
    if configured_target_count > 0 and missing_target_count == configured_target_count:
        errors.append("all_python_probe_targets_missing")
    if targets_with_missing_fields:
        errors.append("python_probe_required_fields_missing")
    if exception_targets:
        errors.append("python_probe_exception_events")
    return errors


def trace_channel_missing_channels(
    channels_enabled: set[str],
    coverage: dict[str, Any],
) -> list[str]:
    """Return enabled capture channels that produced no files."""

    required = {
        "torch": "torch_trace_files",
        "ld_preload": "ld_preload_trace_files",
        "python_probe": "python_probe_trace_files",
    }
    return sorted(
        channel
        for channel, count_key in required.items()
        if channel in channels_enabled and int(coverage.get(count_key) or 0) <= 0
    )


def full_dag_channels_enabled(channels_enabled: set[str]) -> bool:
    """Return whether the run declares the three-channel full-DAG contract."""

    return {"torch", "ld_preload", "python_probe"}.issubset(channels_enabled)


def configured_targets(profiling: dict[str, Any]) -> dict[str, dict[str, Any]]:
    """Reconstruct configured targets selected by requested consumers."""

    targets: dict[str, dict[str, Any]] = {}
    requested = {str(consumer) for consumer in profiling.get("python_consumers") or [] if isinstance(consumer, str)}
    if not requested:
        return targets
    catalog_path = profiling.get("python_target_catalog")
    path = (
        map_repo_path(Path(str(catalog_path)))
        if isinstance(catalog_path, str)
        else ROOT_DIR / "configs/profiling/hicache_probe_targets.json"
    )
    if not path.is_file():
        return targets
    raw_targets = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(raw_targets, list):
        return targets
    for item in raw_targets:
        if not isinstance(item, dict):
            continue
        fact = item.get("fact") if isinstance(item.get("fact"), dict) else {}
        consumers = fact.get("consumers") if isinstance(fact.get("consumers"), list) else []
        if not (requested & {str(consumer) for consumer in consumers if isinstance(consumer, str)}):
            continue
        target_id = item.get("id")
        if isinstance(target_id, str) and target_id:
            targets[target_id] = item
    return targets


def existing_dir_files(raw_dir: Any, pattern: str) -> list[Path]:
    """Discover readable files below a directory stored in the manifest."""

    if not isinstance(raw_dir, str):
        return []
    directory = map_repo_path(Path(raw_dir))
    if not directory.is_dir():
        return []
    return sorted(item for item in directory.glob(pattern) if item.is_file())


def discover_workload_report(run_dir: Path) -> Path | None:
    """Locate the latest workload report produced by a profiling run."""

    if not run_dir.is_dir():
        return None
    candidates = sorted(run_dir.glob("bench/**/workload_report.json"))
    return candidates[-1] if candidates else None


def load_run_config(manifest: dict[str, Any], run_dir: Path) -> dict[str, Any]:
    """Load the run-local expanded config, returning an empty object on failure."""

    raw_path = manifest.get("config_path")
    config_path = map_repo_path(Path(str(raw_path))) if raw_path else run_dir / "config.json"
    if not config_path.is_file():
        return {}
    try:
        payload = json.loads(config_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return {}
    return payload if isinstance(payload, dict) else {}


def load_python_probe_events(paths: list[Path]) -> list[dict[str, Any]]:
    """Load Python probe Chrome events from readable sidecar files."""

    events: list[dict[str, Any]] = []
    for path in paths:
        raw_events, _status = load_chrome_trace_events(path, auto_repair=True)
        events.extend(event for event in raw_events if is_python_probe_trace_event(event))
    return events


def is_python_probe_trace_event(event: dict[str, Any]) -> bool:
    """Return whether a Chrome event belongs to the Python probe channel."""

    args = event.get("args") if isinstance(event.get("args"), dict) else {}
    return (
        event.get("cat") == "python_probe"
        or str(args.get("domain") or "") == "python_probe"
        or str(event.get("name") or "").startswith("hicache_")
    )


def resolve_path(value: str) -> Path:
    """Resolve a CLI path against the repository when it is relative."""

    path = Path(value).expanduser()
    if path.is_absolute():
        return map_repo_path(path)
    return ROOT_DIR / path


def resolve_output_path(value: str | None, manifest_path: Path, result: dict[str, Any]) -> Path:
    """Resolve the explicit or default generic audit output path."""

    if value:
        return resolve_path(value)
    run_dir = Path(str(result.get("run_dir") or manifest_path.parent))
    return run_dir / "profile_artifact_audit.json"


def _add_optional(values: set[str], value: Any) -> None:
    """Add a non-null value to a normalized string set."""

    if value is not None:
        values.add(str(value))


if __name__ == "__main__":
    raise SystemExit(main())
