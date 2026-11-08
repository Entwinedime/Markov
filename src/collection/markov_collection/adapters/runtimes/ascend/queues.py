"""Resolve actual queue submissions and transport services from native driver evidence."""

from bisect import bisect_left, bisect_right
from collections import defaultdict
from dataclasses import dataclass


class NativeQueues:
    def __init__(self, runtime: list) -> None:
        queues = {}
        self.submissions = []
        calls = defaultdict(list)
        for native in sorted(runtime, key=lambda value: value.begin):
            row = native.record
            if not row["api"].startswith("halSq"):
                continue
            key = row["device"], row["task_system"], row["queue"]
            if row["api"] == "halSqCqAllocate":
                queues[key] = row["stream"]
            elif row["api"] == "halSqCqFree":
                del queues[key]
            elif row["api"] == "halSqTaskSend":
                stream = queues[key]
                self.submissions.append((stream, native))
                calls[native.thread, stream].append(native)
        self.calls = {key: ([call.begin for call in values], values) for key, values in calls.items()}

    def window(self, api, stream: int) -> tuple[int, int]:
        starts, calls = self.calls[api.thread, stream]
        selected = calls[bisect_left(starts, api.begin):bisect_right(starts, api.end)]
        if not selected or any(call.end > api.end for call in selected):
            raise ValueError("SDK command group lacks contained native queue submissions")

        # A launch wrapper can include a later synchronization; its return is not the device submission boundary.
        return selected[0].begin, max(call.end for call in selected)


@dataclass
class ServiceExecution:
    scope: int
    thread: int
    begin: int
    end: int
    stream: int | None = None


class ServiceExecutions:
    def __init__(self, scopes: list[dict], queues: NativeQueues) -> None:
        self.executions = [
            ServiceExecution(scope["id"], call["thread"], call["begin_raw_ns"], call["end_raw_ns"])
            for scope in scopes for call in scope["service"]["executions"]
        ]
        self.submissions = {}
        for stream, native in queues.submissions:
            row = native.record
            if row["service"] < 0:
                continue
            matches = [
                (index, call) for index, call in enumerate(self.executions)
                if call.scope == row["service"] and call.thread == native.thread
                and call.begin <= native.begin and native.end <= call.end
            ]
            if len(matches) != 1:
                raise ValueError("native queue submission lacks one explicit service execution")
            (index, call), = matches
            if call.stream is not None and call.stream != stream:
                raise ValueError("one service execution uses multiple queues; separate completion boundaries are required")
            call.stream = stream
            self.submissions.setdefault(stream, []).append((native, index))

        self.streams = {call.stream for call in self.executions}
        if None in self.streams:
            raise ValueError("transport execution has no native queue submission evidence")
        if any(stream in self.streams and native.record["service"] < 0 for stream, native in queues.submissions):
            raise ValueError("a transport queue also carries work outside its declared services")

    def assign(self, tasks: list) -> None:
        grouped = {stream: [] for stream in self.streams}
        for task in tasks:
            if task.stream in grouped:
                grouped[task.stream].append(task)
        for stream, submissions in self.submissions.items():
            if any(left[0].end > right[0].begin for left, right in zip(submissions, submissions[1:])):
                raise ValueError("concurrent private-queue submissions have no established order")

            # This transport submits one SQE per driver call; its registered queue supplies correspondence, not host/device clocks.
            payload = sorted(grouped[stream], key=lambda task: task.begin)
            if len(payload) != len(submissions):
                raise ValueError(f"private queue {stream} submission/task coverage differs: {len(submissions)}/{len(payload)}")
            for task, (_, execution) in zip(payload, submissions, strict=True):
                task.service = execution
