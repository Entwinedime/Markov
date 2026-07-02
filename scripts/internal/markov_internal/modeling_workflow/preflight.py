"""validation 前置检查。"""

from __future__ import annotations

from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any

from ..common.io import write_json
from .progress import count_text

if TYPE_CHECKING:
    from .context import WorkflowContext


class PreflightCheck(ABC):
    """模型运行规划前必须完成的 profile 输入检查。"""

    name: str

    @abstractmethod
    def run(self, context: "WorkflowContext") -> dict[str, Any]:
        """执行检查并返回 summary payload。"""


@dataclass
class PreflightRunner:
    """执行选中 validation 请求的 preflight check。"""

    context: "WorkflowContext"
    check_types: list[type[PreflightCheck]]
    summaries: dict[str, Any] = field(default_factory=dict)

    def run(self) -> dict[str, Any]:
        """运行去重后的 preflight 检查。"""

        ordered_checks = self._ordered_checks()
        progress = self.context.reporter.start_stage(
            "preflight",
            len(ordered_checks),
            f"validations {','.join(self.context.options.validations)}",
            unit="check",
        )
        for check_type in ordered_checks:
            check = check_type()
            self.summaries[check.name] = check.run(self.context)
            progress.advance(self._running_metrics())
        report = self._report(ordered_checks)
        write_json(self.context.artifacts.preflight_summary_path, self._compact_report(report))
        status, summary = self._done_summary(report)
        progress.finish(status, summary)
        return report

    def _ordered_checks(self) -> list[type[PreflightCheck]]:
        checks: dict[str, type[PreflightCheck]] = {}
        for check_type in self.check_types:
            checks.setdefault(check_type.name, check_type)
        return list(checks.values())

    def _running_metrics(self) -> dict[str, Any]:
        ready = sum(1 for summary in self.summaries.values() if summary.get("ready") is True)
        return {"ready": count_text(ready, len(self.summaries))}

    def _report(self, ordered_checks: list[type[PreflightCheck]]) -> dict[str, Any]:
        return {
            "schema": "trace_sim.modeling_workflow.preflight.v1",
            "selected_validations": list(self.context.options.validations),
            "selected_checks": [check_type.name for check_type in ordered_checks],
            "run_count": len(self.context.runs),
            "ready": all(bool(summary.get("ready")) for summary in self.summaries.values()) if self.summaries else True,
            "checks": self.summaries,
        }

    def _compact_report(self, report: dict[str, Any]) -> dict[str, Any]:
        checks = report.get("checks") if isinstance(report.get("checks"), dict) else {}
        compact_checks: dict[str, Any] = {}
        for name, summary in checks.items():
            if not isinstance(summary, dict):
                continue
            compact = {key: value for key, value in summary.items() if key != "rows"}
            if "rows" in summary:
                compact["row_count"] = len(summary.get("rows") or [])
            compact_checks[name] = compact
        return {**report, "checks": compact_checks}

    def _done_summary(self, report: dict[str, Any]) -> tuple[str, str]:
        checks = report.get("checks") if isinstance(report.get("checks"), dict) else {}
        status = "OK" if report.get("ready") is True else "CHECK"
        parts = [f"{len(checks)} checks"]
        for name, summary in checks.items():
            if not isinstance(summary, dict):
                continue
            if name == "full_dag_trace_channels":
                parts.append("full-dag " + count_text(summary.get("full_trace_ready_count"), summary.get("run_count")))
            elif name == "hicache_state_inputs":
                parts.append(
                    "hicache " + count_text(summary.get("workflow_input_ready_count"), summary.get("run_count"))
                )
            else:
                parts.append(f"{name} ready={summary.get('ready')}")
        return status, " | ".join(parts)
