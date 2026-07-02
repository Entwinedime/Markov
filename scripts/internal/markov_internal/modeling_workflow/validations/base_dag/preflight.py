"""DAG trace 通道前置检查。"""

from __future__ import annotations

from typing import Any

from ....audit.profile_artifacts import audit_profile_artifacts
from ....common.io import write_json
from markov_internal.common.naming import safe_slug
from ...preflight import PreflightCheck


class DagTracePreflightCheck(PreflightCheck):
    """检查 DAG 验证所需的 torch、LD_PRELOAD 和 Python probe 通道。"""

    name = "full_dag_trace_channels"

    def run(self, context: Any) -> dict[str, Any]:
        """对已选择 profile run 执行 full-DAG trace 通道审计。"""

        rows: list[dict[str, Any]] = []
        root = context.artifacts.preflight_dir / self.name
        for run in context.runs:
            audit_path = root / safe_slug(run.input_id) / safe_slug(run.config_id) / "profile_artifact_audit.json"
            audit = audit_profile_artifacts(run.manifest_path)
            write_json(audit_path, audit)
            coverage = (
                audit.get("trace_channel_coverage") if isinstance(audit.get("trace_channel_coverage"), dict) else {}
            )
            errors = audit.get("artifact_errors") if isinstance(audit.get("artifact_errors"), list) else []
            full_trace_ready = (
                int(coverage.get("torch_trace_files") or 0) > 0
                and int(coverage.get("ld_preload_trace_files") or 0) > 0
                and int(coverage.get("python_probe_trace_files") or 0) > 0
                and "trace_channel_missing" not in errors
                and "sidecar_only_trace" not in errors
            )
            rows.append(
                {
                    "label": run.label,
                    "run_id": run.run_id,
                    "config_id": run.config_id,
                    "input_id": run.input_id,
                    "manifest_path": str(run.manifest_path),
                    "audit_path": str(audit_path),
                    "artifact_ready": audit.get("artifact_ready"),
                    "full_trace_ready": full_trace_ready,
                    "trace_channel_coverage": coverage,
                    "missing_trace_channels": audit.get("missing_trace_channels", []),
                    "artifact_errors": errors,
                    "configured_target_count": audit.get("configured_target_count"),
                    "observed_target_count": audit.get("observed_target_count"),
                }
            )
        return summarize_dag_trace_preflight(rows)


def summarize_dag_trace_preflight(rows: list[dict[str, Any]]) -> dict[str, Any]:
    """汇总 DAG trace 通道前置检查结果。"""

    run_count = len(rows)
    full_trace_ready_count = sum(1 for row in rows if row.get("full_trace_ready") is True)
    artifact_ready_count = sum(1 for row in rows if row.get("artifact_ready") is True)
    missing_channel_counts: dict[str, int] = {}
    blocker_counts: dict[str, int] = {}
    for row in rows:
        for channel in row.get("missing_trace_channels") or []:
            missing_channel_counts[str(channel)] = missing_channel_counts.get(str(channel), 0) + 1
        for error in row.get("artifact_errors") or []:
            blocker_counts[str(error)] = blocker_counts.get(str(error), 0) + 1
    return {
        "schema": "trace_sim.modeling_workflow.preflight.full_dag_trace_channels.v1",
        "check": DagTracePreflightCheck.name,
        "run_count": run_count,
        "artifact_ready_count": artifact_ready_count,
        "full_trace_ready_count": full_trace_ready_count,
        "ready": run_count > 0 and full_trace_ready_count == run_count,
        "missing_trace_channel_counts": dict(sorted(missing_channel_counts.items())),
        "blocker_counts": dict(sorted(blocker_counts.items())),
        "rows": rows,
    }
