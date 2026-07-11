"""Preflight contracts evaluated before model-run planning."""

from __future__ import annotations

from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any

from ..common.io import write_json
from .progress import count_text

if TYPE_CHECKING:
    from .context import WorkflowContext


class PreflightCheck(ABC):
    """Inspect profile inputs required by one or more validation objects."""

    name: str
    detail_fields: tuple[str, ...] = ("rows",)

    @abstractmethod
    def run(self, context: WorkflowContext) -> dict[str, Any]:
        """Execute the check and return its complete in-memory summary."""

    def progress_text(self, summary: dict[str, Any]) -> str:
        """Render the compact result shown on the final preflight line."""

        return f"{self.name} ready={summary.get('ready')}"

    def compact_summary(self, summary: dict[str, Any]) -> dict[str, Any]:
        """Remove per-run details from the top-level persisted summary."""

        compact = dict(summary)
        for field_name in self.detail_fields:
            compact.pop(field_name, None)
        return compact


@dataclass
class PreflightRunner:
    """Execute and aggregate preflight checks requested by validations."""

    context: WorkflowContext
    check_types: list[type[PreflightCheck]]
    summaries: dict[str, Any] = field(default_factory=dict)

    def run(self) -> dict[str, Any]:
        """Run each distinct check once and retain full details for planning."""

        ordered_checks = self._ordered_checks()
        progress = self.context.reporter.start_stage(
            "preflight",
            len(ordered_checks),
            f"validations {','.join(self.context.options.validations)}",
            unit="check",
        )
        for check in ordered_checks:
            self.summaries[check.name] = check.run(self.context)
            progress.advance(self._running_metrics())
        report = self._report(ordered_checks)
        write_json(self.context.artifacts.preflight_summary_path, self._compact_report(report, ordered_checks))
        status, summary = self._done_summary(report, ordered_checks)
        progress.finish(status, summary)
        return report

    def _ordered_checks(self) -> list[PreflightCheck]:
        checks: dict[str, type[PreflightCheck]] = {}
        for check_type in self.check_types:
            existing = checks.get(check_type.name)
            if existing is not None and existing is not check_type:
                raise ValueError(
                    f"Conflicting preflight check name {check_type.name!r}: "
                    f"{existing.__module__}.{existing.__qualname__} and "
                    f"{check_type.__module__}.{check_type.__qualname__}"
                )
            checks.setdefault(check_type.name, check_type)
        return [check_type() for check_type in checks.values()]

    def _running_metrics(self) -> dict[str, Any]:
        ready = sum(1 for summary in self.summaries.values() if summary.get("ready") is True)
        return {"ready": count_text(ready, len(self.summaries))}

    def _report(self, ordered_checks: list[PreflightCheck]) -> dict[str, Any]:
        return {
            "schema": "trace_sim.modeling_workflow.preflight.v1",
            "selected_validations": list(self.context.options.validations),
            "selected_checks": [check.name for check in ordered_checks],
            "run_count": len(self.context.runs),
            "ready": all(bool(summary.get("ready")) for summary in self.summaries.values()) if self.summaries else True,
            "checks": self.summaries,
        }

    def _compact_report(
        self,
        report: dict[str, Any],
        ordered_checks: list[PreflightCheck],
    ) -> dict[str, Any]:
        checks = report.get("checks") if isinstance(report.get("checks"), dict) else {}
        compact_checks = {
            check.name: check.compact_summary(checks[check.name])
            for check in ordered_checks
            if isinstance(checks.get(check.name), dict)
        }
        return {**report, "checks": compact_checks}

    def _done_summary(
        self,
        report: dict[str, Any],
        ordered_checks: list[PreflightCheck],
    ) -> tuple[str, str]:
        checks = report.get("checks") if isinstance(report.get("checks"), dict) else {}
        status = "OK" if report.get("ready") is True else "CHECK"
        parts = [f"{len(checks)} checks"]
        for check in ordered_checks:
            summary = checks.get(check.name)
            if isinstance(summary, dict):
                parts.append(check.progress_text(summary))
        return status, " | ".join(parts)
