"""Bound child-process cleanup and keep failure context alongside the complete log."""

import subprocess
from collections import deque
from pathlib import Path


def terminate(process: subprocess.Popen) -> None:
    if process.poll() is not None:
        return

    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


class CommandFailed(RuntimeError):
    def __init__(self, returncode: int, log: Path, begin: int = 0) -> None:
        self.returncode = returncode
        self.stderr = self.context(log, begin)
        super().__init__(f"command failed ({returncode}): {self.stderr}\nlog: {log}")

    @staticmethod
    def context(path: Path, begin: int = 0) -> str:
        with path.open() as log:
            log.seek(begin)
            lines = (line.strip()[:600] for line in log)
            lines = (line for line in lines if line)
            first = next(lines, "")
            tail = deque(lines, maxlen=12)
        return "\n".join((first, *tail))
