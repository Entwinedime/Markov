"""Compact source-fact preflight for HiCache prediction."""

from __future__ import annotations

from pathlib import Path
from typing import Any

from markov_internal.common.io import write_json
from markov_internal.common.naming import safe_slug

from ....types import ProfileRunRef
from .profile import audit_hicache_profile


def build_state_input_preflight_report(
    runs: list[ProfileRunRef],
    *,
    audit_dir: Path,
    retain_details: bool = False,
) -> dict[str, Any]:
    """Audit each selected source exactly once and return the planning gate."""

    rows = [_audit_run(run, audit_dir, retain_details=retain_details) for run in runs]
    ready_count = sum(row["workflow_input_ready"] is True for row in rows)
    return {
        "stage": "preflight",
        "run_count": len(rows),
        "workflow_input_ready": bool(rows) and ready_count == len(rows),
        "workflow_input_ready_count": ready_count,
        "state_model_input_ready_count": sum(row["state_model_input_ready"] is True for row in rows),
        "artifact_ready_count": sum(row["artifact_ready"] is True for row in rows),
        "runs": rows,
    }


def _audit_run(run: ProfileRunRef, audit_dir: Path, *, retain_details: bool) -> dict[str, Any]:
    audit = audit_hicache_profile(run.manifest_path)
    audit_path = audit_dir / f"{safe_slug(run.run_id)}.hicache_profile_audit.json"
    if retain_details:
        write_json(audit_path, audit)
    return {
        "run_id": run.run_id,
        "config_id": run.config_id,
        "input_id": run.input_id,
        "manifest_path": str(run.manifest_path),
        "hicache_profile_audit_path": str(audit_path) if retain_details else None,
        "python_probe_file_count": len(run.python_probe_files),
        "requested_consumers": audit.get("requested_consumers", []),
        "artifact_ready": bool(audit.get("artifact_ready")),
        "artifact_errors": audit.get("artifact_errors", []),
        "state_model_input_ready": bool(audit.get("state_model_input_ready")),
        "state_model_input_errors": audit.get("state_model_input_errors", []),
        "workflow_input_ready": bool(audit.get("workflow_input_ready")),
        "workflow_input_errors": audit.get("workflow_input_errors", []),
    }
