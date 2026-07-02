"""HiCache state-model input preflight report 构造器。"""

from __future__ import annotations

import collections
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable

from markov_internal.common.io import write_json
from markov_internal.common.naming import safe_slug
from ....types import ProfileRunRef
from .profile import HiCacheProfileAuditOptions, audit_hicache_profile
from .readiness import (
    normalize_forced_token_preflight,
    public_preflight_row,
    state_model_input_ready,
    summarize_forced_token_input_group,
    workflow_input_ready,
)
from .workload_signature import WorkloadSignatureBuilder, summarize_workload_sequence_input_group


@dataclass(frozen=True)
class StateInputPreflightOptions:
    """HiCache state input preflight 的执行开关。"""

    require_validation_evidence: bool = False
    validate_diagnostic_coverage: bool = False
    require_cross_config_contract: bool = False
    show_workload_sequence: bool = False


@dataclass
class StateInputPreflightReportBuilder:
    """构造 HiCache state input preflight report。"""

    runs: list[ProfileRunRef]
    output_dir: Path
    options: StateInputPreflightOptions
    audit_dir: Path | None = None
    summary_path: Path | None = None
    on_row: Callable[[dict[str, Any]], None] | None = None

    def build(self) -> dict[str, Any]:
        """构造完整 report 并写出 compact summary。"""

        rows = self._run_rows()
        signature_by_input = self._input_groups(rows)
        all_ready = all(row["workflow_input_ready"] for row in rows) and all(
            row["input_contract_ready"] for row in signature_by_input.values()
        )
        report = self._report(rows, signature_by_input, all_ready)
        write_json(self.summary_path or self._default_summary_path(), compact_preflight_report(report))
        return report

    def _run_rows(self) -> list[dict[str, Any]]:
        rows: list[dict[str, Any]] = []
        preflight_dir = self.audit_dir or self.output_dir / "artifacts" / "preflight"
        audit_options = HiCacheProfileAuditOptions(
            validate_forced_token=self.options.require_cross_config_contract,
            validate_oracle_evidence=self.options.require_validation_evidence,
            validate_diagnostic_coverage=self.options.validate_diagnostic_coverage,
        )
        for run in self.runs:
            row = self._run_row(run, preflight_dir, audit_options)
            rows.append(row)
            if self.on_row is not None:
                self.on_row(row)
        return rows

    def _run_row(
        self,
        run: ProfileRunRef,
        preflight_dir: Path,
        audit_options: HiCacheProfileAuditOptions,
    ) -> dict[str, Any]:
        profile_audit_path = preflight_dir / f"{safe_slug(run.run_id)}.hicache_profile_audit.json"
        profile_audit = audit_hicache_profile(run.manifest_path, options=audit_options)
        write_json(profile_audit_path, profile_audit)
        workload_signature = WorkloadSignatureBuilder(
            run,
            include_sequence_events=self.options.show_workload_sequence,
        ).build()
        forced_token_preflight = (
            normalize_forced_token_preflight(profile_audit) if self.options.require_cross_config_contract else None
        )
        state_ready = state_model_input_ready(profile_audit, workload_signature)
        workflow_ready = workflow_input_ready(
            profile_audit,
            state_ready,
            require_validation_evidence=self.options.require_validation_evidence,
        )
        row = self._base_row(run, profile_audit_path, profile_audit, workload_signature, state_ready, workflow_ready)
        self._add_optional_row_fields(row, profile_audit, forced_token_preflight)
        return row

    def _base_row(
        self,
        run: ProfileRunRef,
        profile_audit_path: Path,
        profile_audit: dict[str, Any],
        workload_signature: dict[str, Any],
        state_ready: bool,
        workflow_ready: bool,
    ) -> dict[str, Any]:
        row = {
            "run_id": run.run_id,
            "config_id": run.config_id,
            "input_id": run.input_id,
            "input_class": run.input_class,
            "manifest_path": str(run.manifest_path),
            "run_dir": str(run.run_dir),
            "hicache_profile_audit_path": str(profile_audit_path),
            "python_probe_file_count": len(run.python_probe_files),
            "requested_consumers": profile_audit.get("requested_consumers", []),
            "canonical_request_count": workload_signature["request_event_count"],
            "canonical_workload_signature": workload_signature["signature"],
            "canonical_workload_ready": workload_signature["ready"],
            "canonical_workload_sequence_signature": workload_signature["sequence_signature"],
            "canonical_workload_sequence_ready": workload_signature["sequence_ready"],
            "artifact_ready": bool(profile_audit.get("artifact_ready")),
            "artifact_errors": profile_audit.get("artifact_errors", []),
            "state_model_input_ready": state_ready,
            "state_model_input_errors": profile_audit.get("state_model_input_errors", []),
            "workflow_input_ready": workflow_ready,
            "workflow_input_errors": profile_audit.get("workflow_input_errors", []),
        }
        if self.options.show_workload_sequence:
            row["_workload_sequence_events"] = workload_signature.get("sequence_events", [])
        return row

    def _add_optional_row_fields(
        self,
        row: dict[str, Any],
        profile_audit: dict[str, Any],
        forced_token_preflight: dict[str, Any] | None,
    ) -> None:
        if self.options.validate_diagnostic_coverage:
            row.update(
                {
                    "strict_diagnostic_coverage_ready": profile_audit.get("strict_diagnostic_coverage_ready"),
                    "diagnostic_coverage_errors": profile_audit.get("diagnostic_coverage_errors", []),
                }
            )
        if self.options.require_validation_evidence:
            row.update(
                {
                    "validator_evidence_ready": profile_audit.get("validator_evidence_ready"),
                    "validator_evidence_errors": profile_audit.get("validator_evidence_errors", []),
                }
            )
        if forced_token_preflight is not None:
            row.update(
                {
                    "forced_token_enabled": bool(forced_token_preflight.get("enabled")),
                    "forced_token_ready": bool(forced_token_preflight.get("ready")),
                    "forced_token_plan_ready": bool(forced_token_preflight.get("plan_ready")),
                    "forced_token_bundle_ready": bool(forced_token_preflight.get("bundle_ready")),
                    "forced_token_mode": forced_token_preflight.get("mode"),
                    "forced_token_plan_sha256": forced_token_preflight.get("plan_sha256"),
                    "forced_token_bundle_sha256": forced_token_preflight.get("bundle_sha256"),
                    "forced_token_bundle_id": forced_token_preflight.get("bundle_id"),
                    "forced_token_bundle_path": forced_token_preflight.get("bundle_path"),
                }
            )

    def _input_groups(self, rows: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
        by_input: dict[str, list[dict[str, Any]]] = collections.defaultdict(list)
        for row in rows:
            by_input[str(row["input_id"])].append(row)
        return {input_id: self._input_group(input_id, input_rows) for input_id, input_rows in sorted(by_input.items())}

    def _input_group(self, input_id: str, input_rows: list[dict[str, Any]]) -> dict[str, Any]:
        signatures = sorted(
            {str(row["canonical_workload_signature"]) for row in input_rows if row["canonical_workload_signature"]}
        )
        forced_token_summary = (
            summarize_forced_token_input_group(input_rows) if self.options.require_cross_config_contract else {}
        )
        sequence_summary = summarize_workload_sequence_input_group(
            input_rows,
            include_details=self.options.show_workload_sequence,
        )
        signature_match = len(signatures) == 1 if self.options.require_cross_config_contract else None
        row = {
            "input_id": input_id,
            "run_count": len(input_rows),
            "config_ids": sorted(str(item["config_id"]) for item in input_rows),
            "signature_count": len(signatures),
            "signature_match": signature_match,
            "signatures": signatures,
            "canonical_workload_ready": all(item.get("canonical_workload_ready") for item in input_rows),
            **sequence_summary,
            **forced_token_summary,
        }
        row["input_contract_ready"] = self._input_group_ready(row)
        return row

    def _input_group_ready(self, row: dict[str, Any]) -> bool:
        input_ready = bool(row["canonical_workload_ready"])
        if not self.options.require_cross_config_contract:
            return input_ready
        return input_ready and (
            row["signature_match"] is True
            and row["forced_token_enabled_count"] == row["run_count"]
            and row["forced_token_plan_signature_match"]
            and row["forced_token_bundle_signature_match"]
        )

    def _report(
        self,
        rows: list[dict[str, Any]],
        signature_by_input: dict[str, dict[str, Any]],
        all_workflow_inputs_ready: bool,
    ) -> dict[str, Any]:
        report = {
            "schema": "trace_sim.hicache.state_model.preflight.v1",
            "stage": "preflight",
            "preflight_options": {
                "require_validation_evidence": self.options.require_validation_evidence,
                "validate_diagnostic_coverage": self.options.validate_diagnostic_coverage,
                "require_cross_config_contract": self.options.require_cross_config_contract,
                "show_workload_sequence": self.options.show_workload_sequence,
            },
            "run_count": len(rows),
            "config_ids": sorted({run.config_id for run in self.runs}),
            "input_ids": sorted({run.input_id for run in self.runs}),
            "workflow_input_ready": all_workflow_inputs_ready,
            "workflow_input_ready_count": sum(1 for row in rows if row["workflow_input_ready"]),
            "state_model_input_ready_count": sum(1 for row in rows if row["state_model_input_ready"]),
            "artifact_ready_count": sum(1 for row in rows if row["artifact_ready"]),
            "sequence_check_display_enabled": self.options.show_workload_sequence,
            "input_sequence_match_count": sum(
                1 for item in signature_by_input.values() if item.get("sequence_match") is True
            ),
            "input_workload_signatures": signature_by_input,
            "runs": [public_preflight_row(row) for row in rows],
            "note": (
                "workflow_input_ready gates modeling workflow execution. "
                "state_model_input_ready covers workload identity state facts, token dictionary/span, "
                "and workload identity signatures. Validation-only fields are omitted when disabled."
            ),
        }
        if self.options.validate_diagnostic_coverage:
            report["strict_diagnostic_coverage_ready_count"] = sum(
                1 for row in rows if row.get("strict_diagnostic_coverage_ready") is True
            )
        return report

    def _default_summary_path(self) -> Path:
        return self.output_dir / "artifacts" / "preflight" / "hicache_state_inputs" / "summary.json"


def build_state_input_preflight_report(
    runs: list[ProfileRunRef],
    output_dir: Path,
    *,
    audit_dir: Path | None = None,
    summary_path: Path | None = None,
    on_row: Callable[[dict[str, Any]], None] | None = None,
    require_validation_evidence: bool = False,
    validate_diagnostic_coverage: bool = False,
    require_cross_config_contract: bool = False,
    show_workload_sequence: bool = False,
) -> dict[str, Any]:
    """构造 HiCache state-input preflight gate 并写出 per-run audits。"""

    options = StateInputPreflightOptions(
        require_validation_evidence=require_validation_evidence,
        validate_diagnostic_coverage=validate_diagnostic_coverage,
        require_cross_config_contract=require_cross_config_contract,
        show_workload_sequence=show_workload_sequence,
    )
    return StateInputPreflightReportBuilder(
        runs=runs,
        output_dir=output_dir,
        options=options,
        audit_dir=audit_dir,
        summary_path=summary_path,
        on_row=on_row,
    ).build()


def compact_preflight_report(report: dict[str, Any]) -> dict[str, Any]:
    """构造写入 stage summary 的 compact payload。"""

    compact = {
        "schema": report.get("schema"),
        "stage": report.get("stage"),
        "preflight_options": report.get("preflight_options", {}),
        "run_count": report.get("run_count"),
        "config_ids": report.get("config_ids", []),
        "input_ids": report.get("input_ids", []),
        "workflow_input_ready": report.get("workflow_input_ready"),
        "workflow_input_ready_count": report.get("workflow_input_ready_count"),
        "state_model_input_ready_count": report.get("state_model_input_ready_count"),
        "artifact_ready_count": report.get("artifact_ready_count"),
        "sequence_check_display_enabled": report.get("sequence_check_display_enabled"),
        "input_sequence_match_count": report.get("input_sequence_match_count"),
        "input_workload_signatures": compact_input_workload_signatures(report.get("input_workload_signatures")),
        "note": "This summary keeps only stage-level workflow gate fields. Full audits live under artifacts/preflight.",
    }
    if "strict_diagnostic_coverage_ready_count" in report:
        compact["strict_diagnostic_coverage_ready_count"] = report.get("strict_diagnostic_coverage_ready_count")
    return compact


def compact_input_workload_signatures(value: Any) -> dict[str, dict[str, Any]]:
    """压缩 per-input workload contract summary。"""

    inputs = value if isinstance(value, dict) else {}
    result: dict[str, dict[str, Any]] = {}
    for input_id, item in sorted(inputs.items()):
        if not isinstance(item, dict):
            continue
        row = {
            "input_id": str(input_id),
            "run_count": item.get("run_count"),
            "config_ids": item.get("config_ids", []),
            "signature_count": item.get("signature_count"),
            "signature_match": item.get("signature_match"),
            "sequence_signature_count": item.get("sequence_signature_count"),
            "sequence_match": item.get("sequence_match"),
            "canonical_workload_ready": item.get("canonical_workload_ready"),
            "input_contract_ready": item.get("input_contract_ready"),
        }
        if "sequence_check" in item:
            row["sequence_check"] = item.get("sequence_check")
        for key in (
            "forced_token_enabled_count",
            "forced_token_plan_signature_count",
            "forced_token_plan_signature_match",
            "forced_token_bundle_signature_count",
            "forced_token_bundle_signature_match",
            "forced_token_bundle_ids",
        ):
            if key in item:
                row[key] = item.get(key)
        result[str(input_id)] = row
    return result
