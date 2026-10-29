"""Write complete, snapshot-free probe records without a closing-array dependency."""

import json
import os
from pathlib import Path
import threading


class EventWriter:
    def __init__(self, directory: Path):
        self.directory = directory
        self.process = None
        self.stream = None
        self.lock = threading.Lock()

    def write(self, name: str, begin_ns: int, end_ns: int, fields: dict) -> None:
        process = os.getpid()
        event = {
            "name": name,
            "ph": "X",
            "pid": process,
            "tid": threading.get_native_id(),
            "ts": self.microseconds(begin_ns),
            "dur": self.microseconds(end_ns - begin_ns),
            "args": fields,
        }
        line = json.dumps(event, separators=(",", ":"), allow_nan=False) + "\n"
        with self.lock:
            # A fork must never append to its parent's stream.
            if self.process != process:
                if self.stream is not None:
                    self.stream.close()
                self.stream = (self.directory / f"input_facts_{process}.jsonl").open("x", buffering=1, encoding="utf-8")
                self.process = process
            self.stream.write(line)

    @staticmethod
    def microseconds(nanoseconds: int) -> str:
        return f"{nanoseconds // 1000}.{nanoseconds % 1000:03d}"
