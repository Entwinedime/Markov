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

    def inside(self, api, stream: int) -> list:
        starts, calls = self.calls.get((api.thread, stream), ([], []))
        selected = calls[bisect_left(starts, api.begin):bisect_right(starts, api.end)]
        if any(call.end > api.end for call in selected):
            raise ValueError("native queue submission crosses its enclosing API boundary")
        return selected

    def window(self, api, stream: int) -> tuple[int, int]:
        selected = self.inside(api, stream)
        if not selected:
            raise ValueError("SDK command group lacks contained native queue submissions")

        # A launch wrapper can include a later synchronization; its return is not the device submission boundary.
        return selected[0].begin, max(call.end for call in selected)


@dataclass
class ServiceExecution:
    scope: int
    thread: int
    begin: int
    end: int


class ServiceExecutions:
    def __init__(self, scopes: list[dict], queues: NativeQueues) -> None:
        owners = {scope["id"]: scope for scope in scopes}
        self.executions = [
            ServiceExecution(scope["id"], call["thread"], call["begin_counter"], call["end_counter"])
            for scope in scopes for call in scope["service"]["executions"]
        ]
        for call in self.executions:
            interval = owners[call.scope]["host_interval"]
            if not interval["begin_counter"] <= call.begin <= call.end <= interval["end_counter"]:
                raise ValueError("transport execution escapes its synchronous service boundary")
        self.submissions = {}
        queue_owners = {}
        submitted = set()
        for stream, native in queues.submissions:
            row = native.record
            if row["service"] < 0:
                continue
            if type(row["tasks"]) is not int or row["tasks"] < 0:
                raise ValueError("private queue submission requires its observed nonnegative SQE count")
            matches = [
                (index, call) for index, call in enumerate(self.executions)
                if call.scope == row["service"] and call.thread == native.thread
                and call.begin <= native.begin and native.end <= call.end
            ]
            if len(matches) != 1:
                raise ValueError("native queue submission lacks one explicit service execution")
            (index, call), = matches

            # Shared queues must remain ordered by the caller thread retained in the coarse graph.
            owner = owners[call.scope]["thread"]
            if queue_owners.setdefault(stream, owner) != owner:
                raise ValueError("transport queue crosses service callers; its ordering requires an explicit interface")
            submitted.add(index)
            self.submissions.setdefault(stream, []).append((native, index))

        self.streams = set(self.submissions)
        if len(submitted) != len(self.executions):
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

            # Driver submission sizes define correspondence, independently of CPU/device clock calibration.
            payload = sorted(grouped[stream], key=lambda task: task.begin)
            expected = sum(native.record["tasks"] for native, _ in submissions)
            if len(payload) != expected:
                raise ValueError(f"private queue {stream} submission/task coverage differs: {expected}/{len(payload)}")
            position = 0
            for native, execution in submissions:
                count = native.record["tasks"]
                for task in payload[position:position + count]:
                    task.service = self.executions[execution].scope
                position += count
