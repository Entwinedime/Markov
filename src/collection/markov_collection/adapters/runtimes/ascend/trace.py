"""Index native marker, queue and connection evidence without building an operator graph."""

from __future__ import annotations

import json
import struct
from bisect import bisect_right
from collections import defaultdict
from dataclasses import dataclass
from decimal import Decimal
from pathlib import Path
from typing import Any

from markov_collection.adapters.runtimes.ascend.clock import AscendClock, HostCounter
from markov_collection.adapters.runtimes.ascend.queues import NativeQueues, ServiceExecutions


@dataclass
class Interval:
    begin: int
    end: int
    thread: int
    record: dict[str, Any]

    @classmethod
    def read(cls, record: dict[str, Any], counter: HostCounter) -> Interval:
        begin, end = counter.interval(record)
        return cls(begin, end, int(record["tid"]), record)

    def contains(self, other: Interval) -> bool:
        return self.thread == other.thread and self.begin <= other.begin and other.end <= self.end


class ThreadIntervals:
    def __init__(self, intervals: list[Interval]) -> None:
        self.threads: dict[int, tuple[list[int], list[Interval], list[int], list[int]]] = {}
        grouped: dict[int, list[Interval]] = defaultdict(list)
        for interval in intervals:
            grouped[interval.thread].append(interval)
        for thread, values in grouped.items():
            values.sort(key=lambda value: (value.begin, -value.end))
            parents = []
            furthest = []
            active = []
            for index, value in enumerate(values):
                while active and not values[active[-1]].contains(value):
                    active.pop()
                parents.append(active[-1] if active else -1)
                active.append(index)
                furthest.append(max(value.end, furthest[-1] if furthest else value.end))
            self.threads[thread] = [value.begin for value in values], values, parents, furthest

    def enclosing(self, interval: Interval) -> Interval | None:
        if interval.thread not in self.threads:
            return None
        starts, values, parents, _ = self.threads[interval.thread]
        index = bisect_right(starts, interval.begin) - 1
        while index >= 0:
            if values[index].contains(interval):
                return values[index]
            index = parents[index]
        return None

    def inside(self, interval: Interval) -> list[Interval]:
        if interval.thread not in self.threads:
            return []
        starts, values, _, _ = self.threads[interval.thread]
        begin = bisect_right(starts, interval.begin - 1)
        end = bisect_right(starts, interval.end)
        return [value for value in values[begin:end] if interval.contains(value)]

    def overlapping(self, interval: Interval) -> list[Interval]:
        if interval.thread not in self.threads:
            return []
        starts, values, _, furthest = self.threads[interval.thread]
        index = bisect_right(starts, interval.end) - 1
        result = []
        while index >= 0 and furthest[index] >= interval.begin:
            if values[index].end >= interval.begin:
                result.append(values[index])
            index -= 1
        return result


@dataclass
class DeviceTask:
    begin: int
    end: int
    record: dict[str, Any]
    service: int | None = None

    @property
    def stream(self) -> int:
        return int(self.record["args"]["Physic Stream Id"])


@dataclass
class DevicePoint:
    host: Interval
    api: Interval
    task: DeviceTask
    stream: int

class AscendTrace:
    host_waits = frozenset({
        "aclrtSynchronizeStream", "aclrtSynchronizeStreamWithTimeout", "aclrtSynchronizeEvent",
        "aclrtSynchronizeEventWithTimeout", "aclrtSynchronizeDeviceWithTimeout",
    })
    stream_waits = frozenset({"aclrtStreamWaitEvent", "aclrtStreamWaitEventWithTimeout"})

    def __init__(self, profile: Path, runtime: Path, raw: dict, trace: Path) -> None:
        native_points = {index for index, point in enumerate(raw["points"]) if point["side"] == "signal"}
        services = [scope for scope in raw["scopes"] if "service" in scope]
        host_observation = raw["host_observation"]

        # Host counters and stream ordinals establish order; measured RAW time is separate evidence.
        counter = HostCounter(profile)
        device, = profile.glob("PROF_*/device_*")
        self.clock = AscendClock(device, raw["device_clock"])
        rows = json.loads(trace.read_text(), parse_float=Decimal)
        flips = self.task_flips(profile)
        intervals = [self.device_interval(row) if "Task Type" in row.get("args", {}) else Interval.read(row, counter)
                     for row in rows if row.get("ph") == "X"
                     and (row.get("cat") in {"enqueue", "dequeue"} or "connection_id" in row.get("args", {}))]
        self.host_times = {}
        native_intervals = [self.native_interval(row) for row in host_observation["intervals"]]
        self.process = host_observation["process"]
        self.scopes = {}
        self.points = {}
        windows = []
        self.excluded = []
        enqueues, dequeues = [], []
        self.apis: dict[int, Interval] = {}
        self.tasks: dict[int, list[DeviceTask]] = defaultdict(list)
        self.device_tasks = []
        streams = defaultdict(list)
        for interval in [*native_intervals, *intervals]:
            row = interval.record
            name = row["name"]
            if name == "markov.window":
                windows.append(interval)
            elif name.startswith("markov.scope/"):
                identity = int(name.split("/")[1])
                if identity in self.scopes:
                    raise ValueError(f"duplicate semantic scope marker: {identity}")
                self.scopes[identity] = interval
            elif name.startswith("markov.point/"):
                identity = int(name.split("/")[1])
                if identity in self.points:
                    raise ValueError(f"duplicate device point marker: {identity}")
                self.points[identity] = interval

                # Exported native Event.record is business work; only inserted observer fences are auxiliary.
                if identity not in native_points:
                    self.excluded.append(interval)
            elif name in {"markov.observer", "markov.auxiliary"}:
                self.excluded.append(interval)
            elif row.get("cat") == "enqueue":
                enqueues.append(interval)
            elif row.get("cat") == "dequeue":
                dequeues.append(interval)
            elif "Thread Id" in row.get("args", {}) and "connection_id" in row["args"]:
                connection = row["args"]["connection_id"]
                if connection in self.apis:
                    raise ValueError(f"duplicate native API connection: {connection}")
                self.apis[connection] = interval
            elif "Task Type" in row.get("args", {}):
                args = row["args"]
                stream = int(args["Physic Stream Id"])

                # Only an identified runtime flip is bookkeeping; other placeholder SQEs can perform memory work.
                if args["Task Type"] == "PLACE_HOLDER_SQE" and (stream, int(args["Task Id"]), int(args["Batch Id"])) in flips:
                    continue
                streams[stream].append(interval)

        # A zero-time wait still has distinct entry and completion. Wall-clock coincidence must not change graph topology.
        for tasks in streams.values():
            tasks.sort(key=lambda value: (value.begin, value.record["args"]["Batch Id"], value.record["args"]["Task Id"], value.record["args"]["Subtask Id"]))
            for order, interval in enumerate(tasks):
                task = DeviceTask(2 * order, 2 * order + 1, interval.record)
                self.tasks[task.record["args"]["connection_id"]].append(task)
                self.device_tasks.append(task)
        if len(windows) != 1:
            raise ValueError("native recording requires one closed observation window")
        self.window, = windows
        for scope in services:
            identity = scope["id"]
            if identity in self.scopes:
                raise ValueError(f"service has two competing host observations: {identity}")
            interval = scope["host_interval"]
            self.scopes[identity] = self.native_interval({**interval, "thread": scope["thread"], "name": f"markov.scope/{identity}"})
        self.scope_ranges = ThreadIntervals(list(self.scopes.values()))
        self.excluded_ranges = ThreadIntervals(self.excluded)
        self.enqueue_ranges = ThreadIntervals(enqueues)
        self.dequeue_ranges = ThreadIntervals(dequeues)
        self.enqueue = self.queue_index(enqueues)
        self.dequeue = self.queue_index(dequeues)
        self.api_ranges = ThreadIntervals(list(self.apis.values()))

        self.runtime = []
        for line in (runtime / f"{self.process}.jsonl").read_text().splitlines():
            row = json.loads(line)
            if row["result"] != 0:
                raise RuntimeError(f"native runtime call failed: {row['api']} ({row['result']})")
            self.runtime.append(self.native_interval(row))
        self.queues = NativeQueues(self.runtime)
        self.services = ServiceExecutions(services, self.queues)
        for native in self.runtime:
            row = native.record
            if row["api"].startswith("aclrt") and row["stream_status"] == 0 and row["stream"] in self.services.streams:
                raise ValueError("a private transport queue has external runtime operations; explicit ports are required")
        self.services.assign(self.device_tasks)

        self.runtime_ranges = ThreadIntervals(self.runtime)
        self.synchronizations = []
        records = {}
        for native in sorted(self.runtime, key=lambda value: value.begin):
            row = native.record
            if row["api"] == "aclrtRecordEvent":
                records[row["event"]] = native
            elif row["api"] == "aclrtDestroyEvent":
                records.pop(row["event"], None)
            elif row["api"] in self.host_waits | self.stream_waits:

                # Resolve the generation while the handle is live; a later record can reuse the same handle.
                self.synchronizations.append((native, records.get(row.get("event"))))
        self.device_points = {identity: self.device_point(identity) for identity in self.points}
        self.observer_connections = {
            point.api.record["args"]["connection_id"] for identity, point in self.device_points.items() if identity not in native_points
        }

    def native_interval(self, record: dict) -> Interval:
        thread = record["thread"]
        for side in ("begin", "end"):
            for resource in (f"thread/{thread}", f"service/{thread}"):
                self.host_times[resource, record[f"{side}_counter"]] = record[f"{side}_raw_ns"]
        return Interval(record["begin_counter"], record["end_counter"], thread, record)

    def device_interval(self, record: dict) -> Interval:
        begin, end = self.clock.interval(record)
        return Interval(begin, end, int(record["tid"]), record)

    @staticmethod
    def task_flips(profile: Path) -> set[tuple[int, int, int]]:
        directory, = profile.glob("PROF_*/device_*/data")
        files = [path for path in directory.glob("ts_track.data.*.slice_*") if path.name.rsplit("_", 1)[1].isdigit()]
        files.sort(key=lambda path: int(path.name.rsplit("_", 1)[1]))
        data = b"".join(path.read_bytes() for path in files)
        record = struct.Struct("=BBHLQHHHH16B")

        # Match the SDK TsTrack parser's aged-prefix alignment and TS_TASK_FLIP layout.
        records = [row for row in record.iter_unpack(data[len(data) % record.size:]) if row[1] == 14]
        batches: dict[int, int] = defaultdict(int)
        flips = set()
        for row in sorted(records, key=lambda row: row[4]):
            stream, flip, task = row[5], row[6], row[8]

            # DeviceTaskCollector increments the epoch for every flip, including stream destruction (65535).
            batches[stream] += 1
            if flip != 65535:
                flips.add((stream, task, batches[stream]))
        return flips

    @staticmethod
    def queue_index(intervals: list[Interval]) -> dict[int, Interval]:
        result = {}
        for interval in intervals:
            correlation = interval.record["args"]["correlation_id"]
            if correlation in result:
                raise ValueError(f"duplicate native queue correlation: {correlation}")
            result[correlation] = interval
        return result

    @staticmethod
    def anchors(observation: dict):
        for edge in observation["dependencies"]:
            yield edge["source"]
            yield edge["target"]
        for scope in observation["scopes"]:
            for port in scope["ports"]:
                yield port["at"]
                if "wait" in port:
                    yield port["wait"]["begin"]
                if "rendezvous" in port:
                    yield port["rendezvous"]
            for field in ("kv_accesses", "slot_accesses"):
                for access in scope[field]:
                    yield access["start"]
                    yield access["finish"]

    def origin(self, api: Interval) -> Interval:
        dequeue = self.dequeue_ranges.enclosing(api)
        if dequeue is None:
            return api
        return self.enqueue[dequeue.record["args"]["correlation_id"]]

    def owner(self, interval: Interval) -> int | None:
        scope = self.scope_ranges.enclosing(interval)
        return None if scope is None else int(scope.record["name"].split("/")[1])

    def device_point(self, identity: int) -> DevicePoint:
        marker = self.points[identity]
        queues = self.enqueue_ranges.inside(marker)
        if len(queues) != 1:
            raise ValueError(f"device point {identity} does not have one native enqueue")
        dequeue = self.dequeue[queues[0].record["args"]["correlation_id"]]
        calls = [api for api in self.api_ranges.inside(dequeue) if api.record["args"]["id"] == "aclrtRecordEvent"]
        if len(calls) != 1:
            raise ValueError(f"device point {identity} does not identify one record API")
        api = calls[0]
        native = self.runtime_call(api)
        if native is None:
            raise ValueError(f"device point {identity} has no recorded API instance")
        tasks = self.tasks[api.record["args"]["connection_id"]]
        if len(tasks) != 1 or tasks[0].record["args"]["Task Type"] != "EVENT_RECORD":
            raise ValueError(f"device point {identity} does not identify one native event task")
        stream = int(tasks[0].record["args"]["Physic Stream Id"])
        if native.record["stream_status"] != 0 or native.record["stream"] != stream:
            raise ValueError(f"device point {identity} disagrees with its native stream")
        return DevicePoint(marker, api, tasks[0], stream)

    def runtime_call(self, api: Interval) -> Interval | None:

        # Both recordings now use the same counter; the interposer encloses the actual SDK API.
        calls = [native for native in self.runtime_ranges.overlapping(api)
                 if native.contains(api) and native.record["api"] == api.record["args"]["id"]]
        if len(calls) > 1:
            raise ValueError(f"ambiguous native API instance: {api.record['args']['id']}")
        return calls[0] if calls else None

    def native_api(self, native: Interval) -> Interval:
        calls = [api for api in self.api_ranges.inside(native) if api.record["args"]["id"] == native.record["api"]]
        if len(calls) != 1:
            raise ValueError(f"native call does not identify one profiler API: {native.record['api']}")
        return calls[0]

    def auxiliary(self, task: DeviceTask) -> bool:
        api = self.apis.get(task.record["args"]["connection_id"])
        return api is not None and self.excluded_ranges.enclosing(self.origin(api)) is not None

    def outside_window(self, task: DeviceTask) -> bool:
        api = self.apis.get(task.record["args"]["connection_id"])
        if api is None:
            return False
        origin = self.origin(api)
        return origin.end <= self.window.begin or self.window.end <= origin.begin
