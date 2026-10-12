"""Stage-local progress: transient on a terminal, start/end lines in logs."""

from __future__ import annotations

import sys
import time
from typing import TextIO


class StageProgress:
    """Display one sequential stage without carrying UI state in the workflow."""

    def __init__(
        self, name: str, total: int, detail: str = "", *, unit: str = "item", stream: TextIO | None = None
    ) -> None:
        self.name = name
        self.total = total
        self.completed = 0
        self.started_at = time.monotonic()
        self.stream = stream if stream is not None else sys.stdout
        self.is_tty = self.stream.isatty()
        self.dynamic_line_active = False

        units = unit if total == 1 else unit + "s"
        suffix = f" | {detail}" if detail else ""
        self._write("START", f"{total} {units}{suffix}")

    def advance(self, metrics: dict[str, str]) -> None:
        self.completed += 1
        if not self.is_tty:
            return

        filled = min(20, round(20 * self.completed / self.total)) if self.total else 0
        bar = "[" + "#" * filled + "-" * (20 - filled) + "]"
        details = " | ".join(f"{key} {value}" for key, value in metrics.items())
        suffix = f"  {details}" if details else ""
        self._write("RUNNING", f"{count_text(self.completed, self.total):<9} {bar}{suffix}", transient=True)
        self.dynamic_line_active = True

    def finish(self, status: str, summary: str) -> None:
        if self.dynamic_line_active:
            self.stream.write("\n")
            self.dynamic_line_active = False

        seconds = time.monotonic() - self.started_at
        elapsed = f"{seconds:.1f}s" if seconds < 60 else f"{int(seconds // 60)}m{int(seconds % 60):02d}s"
        self._write(status.upper(), f"{summary} | {elapsed}")

    def _write(self, status: str, text: str, *, transient: bool = False) -> None:
        line = f"{self.name:<24} {status:<10} {text}"
        self.stream.write("\r" + line + "\033[K" if transient else line + "\n")
        self.stream.flush()


def count_text(count: int, total: int) -> str:
    return f"{count}/{total}"
