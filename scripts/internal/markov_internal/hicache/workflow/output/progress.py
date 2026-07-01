"""HiCache workflow 面向用户的统一进度输出。"""

from __future__ import annotations

import sys
import time
from dataclasses import dataclass, field
from typing import Any, TextIO

from .summary import summarize_input_contracts


@dataclass
class StageProgress:
    """stage runner 用于报告阶段级进度的生命周期对象。"""

    reporter: WorkflowProgressReporter
    name: str
    total: int
    detail: str = ""
    unit: str = "item"
    started_at: float = field(default_factory=time.monotonic)
    completed: int = 0
    metrics: dict[str, Any] = field(default_factory=dict)

    def advance(self, metrics: dict[str, Any] | None = None, *, step: int = 1) -> None:
        """推进阶段进度，并在 TTY 中刷新运行行。"""

        self.completed = min(self.total, self.completed + step) if self.total else self.completed + step
        if metrics is not None:
            self.metrics = metrics
        self.reporter.update(self)

    def finish(self, status: str, summary: str) -> None:
        """结束阶段并输出最终 summary 行。"""

        self.reporter.finish(self, status, summary)


class WorkflowProgressReporter:
    """所有 workflow stage 共享的 TTY-aware progress reporter。"""

    def __init__(self, stream: TextIO | None = None) -> None:
        """初始化输出流，并检测当前是否支持动态 TTY 刷新。"""

        self.stream = stream or sys.stdout
        self.is_tty = bool(getattr(self.stream, "isatty", lambda: False)())
        self._dynamic_line_active = False

    def start_stage(self, name: str, total: int, detail: str = "", *, unit: str = "item") -> StageProgress:
        """启动阶段并返回进度 handle。"""

        stage = StageProgress(reporter=self, name=name, total=total, detail=detail, unit=unit)
        suffix = f" | {detail}" if detail else ""
        self._write_line(f"{name:<13} start  {value_text(total)} {unit_text(total, unit)}{suffix}")
        return stage

    def update(self, stage: StageProgress) -> None:
        """在 TTY session 中刷新运行中的阶段行。"""

        if not self.is_tty:
            return
        line = self._running_line(stage)
        self.stream.write("\r" + line + "\033[K")
        self.stream.flush()
        self._dynamic_line_active = True

    def finish(self, stage: StageProgress, status: str, summary: str) -> None:
        """写出阶段最终 summary 行。"""

        if self._dynamic_line_active:
            self.stream.write("\n")
            self._dynamic_line_active = False
        elapsed = elapsed_text(time.monotonic() - stage.started_at)
        elapsed_suffix = f" | {elapsed}" if elapsed else ""
        self._write_line(f"{stage.name:<13} {status:<5} {summary}{elapsed_suffix}")

    def _running_line(self, stage: StageProgress) -> str:
        """生成 TTY 动态进度行。"""

        progress = count_text(stage.completed, stage.total)
        metrics = stage_metric_text(stage.metrics)
        metrics_suffix = f"  {metrics}" if metrics else ""
        return f"{stage.name:<13} {progress:<8} {progress_bar(stage.completed, stage.total)}{metrics_suffix}"

    def _write_line(self, text: str) -> None:
        """向输出流写一行并立即 flush。"""

        self.stream.write(text + "\n")
        self.stream.flush()


def quality_running_metrics(rows: list[dict[str, Any]]) -> dict[str, Any]:
    """从已完成 run rows 生成 compact quality counters。"""

    total = len(rows)
    metrics = {
        "workflow": count_text(sum(1 for row in rows if row.get("workflow_input_ready") is True), total),
        "state": count_text(sum(1 for row in rows if row.get("state_model_input_ready") is True), total),
    }
    strict_rows = [row for row in rows if "strict_diagnostic_coverage_ready" in row]
    if strict_rows:
        metrics["strict"] = count_text(
            sum(1 for row in strict_rows if row.get("strict_diagnostic_coverage_ready") is True),
            len(strict_rows),
        )
    return metrics


def quality_done_summary(report: dict[str, Any]) -> tuple[str, str]:
    """生成 quality stage 的状态和 summary 文案。"""

    run_count = report.get("run_count")
    input_contracts = summarize_input_contracts(report)
    status = "OK" if report.get("workflow_input_ready") is True else "CHECK"
    summary = (
        f"{value_text(run_count)} runs | "
        f"workflow {count_text(report.get('workflow_input_ready_count'), run_count)} | "
        f"state {count_text(report.get('state_model_input_ready_count'), run_count)} | "
        f"inputs {count_text(input_contracts['ready_count'], input_contracts['input_count'])}"
    )
    if "strict_diagnostic_coverage_ready_count" in report:
        strict_requested = int(run_count or 0)
        summary += " | strict " + count_text(report.get("strict_diagnostic_coverage_ready_count"), strict_requested)
    if report.get("sequence_check_display_enabled"):
        summary += " | sequence " + count_text(report.get("input_sequence_match_count"), input_contracts["input_count"])
    return status, summary


def final_state_running_metrics(rows: list[dict[str, Any]]) -> dict[str, Any]:
    """从已完成 prediction rows 生成 compact final-state counters。"""

    total = len(rows)
    return {
        "ready": count_text(sum(1 for row in rows if row.get("validation_ready") is True), total),
        "exact": count_text(sum(1 for row in rows if row.get("final_state_match") is True), total),
    }


def final_state_rows_done_summary(rows: list[dict[str, Any]]) -> tuple[str, str]:
    """生成所有 final-state prediction rows 的状态和 summary 文案。"""

    prediction_count = len(rows)
    validation_ready_count = sum(1 for row in rows if row.get("validation_ready") is True)
    final_state_match_count = sum(1 for row in rows if row.get("final_state_match") is True)
    error_count = sum(1 for row in rows if row.get("return_code", 0) != 0)
    status = final_state_status_from_counts(
        prediction_count,
        validation_ready_count,
        final_state_match_count,
        error_count=error_count,
    )
    summary = (
        f"{prediction_count} predictions | "
        f"exact {count_text(final_state_match_count, prediction_count)} | "
        f"ready {count_text(validation_ready_count, prediction_count)}"
    )
    if error_count:
        summary += f" | errors {error_count}"
    return status, summary


def transition_running_metrics(rows: list[dict[str, Any]]) -> dict[str, Any]:
    """从已完成 comparison rows 生成 compact transition counters。"""

    total = len(rows)
    return {
        "ready": count_text(sum(1 for row in rows if row.get("ready") is True), total),
        "exact": count_text(sum(1 for row in rows if row.get("exact") is True), total),
        "count": count_text(sum(1 for row in rows if row.get("transition_count_exact") is True), total),
    }


def transition_done_summary(report: dict[str, Any]) -> tuple[str, str]:
    """生成 transition stage 的状态和 summary 文案。"""

    prediction_count = report.get("prediction_count")
    status = transition_stage_status(report)
    summary = (
        f"{value_text(prediction_count)} predictions | "
        f"exact {count_text(report.get('exact_count'), prediction_count)} | "
        f"ready {count_text(report.get('ready_count'), prediction_count)} | "
        f"count {count_text(report.get('transition_count_exact_count'), prediction_count)}"
    )
    return status, summary


def stage_metric_text(metrics: dict[str, Any]) -> str:
    """渲染运行指标，避免把内部 boolean 字段名平铺给用户。"""

    return "  ".join(f"{key} {value}" for key, value in metrics.items())


def progress_bar(done: int, total: int, *, width: int = 20) -> str:
    """渲染 ASCII 进度条。"""

    if total <= 0:
        filled = 0
    else:
        filled = min(width, int(round(width * done / total)))
    return "[" + "#" * filled + "-" * (width - filled) + "]"


def final_state_stage_status(report: dict[str, Any]) -> str:
    """根据 final-state summary 返回状态 token。"""

    prediction_count = report.get("prediction_count")
    validation_ready_count = report.get("validation_ready_count")
    final_state_match_count = report.get("final_state_match_count")
    if not all(is_plain_int(value) for value in (prediction_count, validation_ready_count, final_state_match_count)):
        return "UNKNOWN"
    return final_state_status_from_counts(
        prediction_count,
        validation_ready_count,
        final_state_match_count,
        error_count=0,
    )


def final_state_status_from_counts(
    prediction_count: int,
    validation_ready_count: int,
    final_state_match_count: int,
    *,
    error_count: int,
) -> str:
    """根据 final-state counters 返回状态 token。"""

    if prediction_count == 0:
        return "EMPTY"
    if error_count:
        return "ERROR"
    if validation_ready_count != prediction_count:
        return "NOT_READY"
    if final_state_match_count == prediction_count:
        return "OK"
    return "MISMATCH"


def transition_stage_status(report: dict[str, Any]) -> str:
    """根据 transition counters 返回状态 token。"""

    prediction_count = report.get("prediction_count")
    ready_count = report.get("ready_count")
    exact_count = report.get("exact_count")
    if not all(is_plain_int(value) for value in (prediction_count, ready_count, exact_count)):
        return "UNKNOWN"
    if prediction_count == 0:
        return "EMPTY"
    if ready_count != prediction_count:
        return "NOT_READY"
    if exact_count == prediction_count:
        return "OK"
    return "MISMATCH"


def count_text(count: Any, total: Any) -> str:
    """渲染 count/total 字段。"""

    return f"{value_text(count)}/{value_text(total)}"


def is_plain_int(value: Any) -> bool:
    """判断 summary count 是否为普通 int，并排除 bool。"""

    return isinstance(value, int) and not isinstance(value, bool)


def value_text(value: Any) -> str:
    """渲染带 unknown 兜底的 compact value。"""

    return "unknown" if value is None else str(value)


def unit_text(count: int, singular: str) -> str:
    """按数量渲染简单英文单位。"""

    return singular if count == 1 else singular + "s"


def elapsed_text(seconds: float) -> str:
    """把 elapsed seconds 渲染为 compact 人读文本。"""

    if seconds < 60:
        return f"{seconds:.1f}s"
    minutes = int(seconds // 60)
    remaining = int(seconds % 60)
    return f"{minutes}m{remaining:02d}s"
