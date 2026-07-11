"""Stable, TTY-aware progress output for internal workflows."""

from __future__ import annotations

import sys
import time
from dataclasses import dataclass, field
from typing import Any, TextIO


STAGE_COLUMN_WIDTH = 24
STATE_COLUMN_WIDTH = 10
COUNT_COLUMN_WIDTH = 9


@dataclass
class StageProgress:
    """Mutable progress handle for one sequential workflow stage."""

    reporter: WorkflowProgressReporter
    name: str
    total: int
    detail: str = ""
    unit: str = "item"
    started_at: float = field(default_factory=time.monotonic)
    completed: int = 0
    metrics: dict[str, Any] = field(default_factory=dict)

    def advance(self, metrics: dict[str, Any] | None = None, *, step: int = 1) -> None:
        """Advance the stage and refresh its dynamic line on a TTY."""

        self.completed = min(self.total, self.completed + step) if self.total else self.completed + step
        if metrics is not None:
            self.metrics = metrics
        self.reporter.update(self)

    def finish(self, status: str, summary: str) -> None:
        """Finish the stage and emit its durable summary line."""

        self.reporter.finish(self, status, summary)


class WorkflowProgressReporter:
    """Render workflow progress without noisy per-item non-TTY output."""

    def __init__(self, stream: TextIO | None = None) -> None:
        """Bind output to `stream` and detect whether transient TTY updates are safe."""

        self.stream = stream or sys.stdout
        self.is_tty = bool(getattr(self.stream, "isatty", lambda: False)())
        self._dynamic_line_active = False

    def start_stage(self, name: str, total: int, detail: str = "", *, unit: str = "item") -> StageProgress:
        """Start a stage and return the handle used for subsequent updates."""

        stage = StageProgress(reporter=self, name=name, total=total, detail=detail, unit=unit)
        suffix = f" | {detail}" if detail else ""
        self._write_line(self._format_line(name, "START", f"{value_text(total)} {unit_text(total, unit)}{suffix}"))
        return stage

    def update(self, stage: StageProgress) -> None:
        """Refresh the transient progress line when the stream is interactive."""

        if not self.is_tty:
            return
        line = self._running_line(stage)
        self.stream.write("\r" + line + "\033[K")
        self.stream.flush()
        self._dynamic_line_active = True

    def finish(self, stage: StageProgress, status: str, summary: str) -> None:
        """Close any transient line and write the final stage summary."""

        if self._dynamic_line_active:
            self.stream.write("\n")
            self._dynamic_line_active = False
        elapsed = elapsed_text(time.monotonic() - stage.started_at)
        elapsed_suffix = f" | {elapsed}" if elapsed else ""
        self._write_line(self._format_line(stage.name, status.upper(), f"{summary}{elapsed_suffix}"))

    def _running_line(self, stage: StageProgress) -> str:
        progress = count_text(stage.completed, stage.total)
        metrics = stage_metric_text(stage.metrics)
        metrics_suffix = f"  {metrics}" if metrics else ""
        payload = f"{progress:<{COUNT_COLUMN_WIDTH}} {progress_bar(stage.completed, stage.total)}{metrics_suffix}"
        return self._format_line(stage.name, "RUNNING", payload)

    def _format_line(self, stage_name: str, state: str, payload: str) -> str:
        return f"{stage_name:<{STAGE_COLUMN_WIDTH}} {state:<{STATE_COLUMN_WIDTH}} {payload}"

    def _write_line(self, text: str) -> None:
        self.stream.write(text + "\n")
        self.stream.flush()


def stage_metric_text(metrics: dict[str, Any]) -> str:
    """Render ordered stage metrics in a compact human-readable form."""

    return " | ".join(f"{key} {value}" for key, value in metrics.items())


def progress_bar(done: int, total: int, *, width: int = 20) -> str:
    """Render a fixed-width ASCII progress bar."""

    if total <= 0:
        filled = 0
    else:
        filled = min(width, round(width * done / total))
    return "[" + "#" * filled + "-" * (width - filled) + "]"


def count_text(count: Any, total: Any) -> str:
    """Render a possibly unknown completed/total count pair."""

    return f"{value_text(count)}/{value_text(total)}"


def value_text(value: Any) -> str:
    """Render missing progress values without inventing a numeric zero."""

    return "unknown" if value is None else str(value)


def unit_text(count: int, singular: str) -> str:
    """Pluralize a simple workflow unit from its count."""

    return singular if count == 1 else singular + "s"


def elapsed_text(seconds: float) -> str:
    """Render elapsed seconds in the workflow's compact display format."""

    if seconds < 60:
        return f"{seconds:.1f}s"
    minutes = int(seconds // 60)
    remaining = int(seconds % 60)
    return f"{minutes}m{remaining:02d}s"
