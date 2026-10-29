"""Write complete, snapshot-free probe records without a closing-array dependency."""

import json
import os
from pathlib import Path
import threading
import time


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

    def span(self, name: str, fields: dict):
        return EventSpan(self, name, fields)


class EventSpan:
    def __init__(self, writer: EventWriter, name: str, fields: dict):
        self.writer, self.name, self.fields = writer, name, fields

    def __enter__(self):
        self.begin = time.time_ns()
        self.cpu = time.thread_time_ns()
        return self.fields

    def __exit__(self, kind, error, traceback):
        end = time.time_ns()
        self.fields.update(
            status="returned" if kind is None else "raised", thread_cpu_ns=time.thread_time_ns() - self.cpu
        )
        self.writer.write(self.name, self.begin, end, self.fields)
