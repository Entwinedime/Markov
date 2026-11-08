"""Index native marker, queue and connection evidence without building an operator graph."""

from __future__ import annotations

import json
import struct
from bisect import bisect_left, bisect_right
from collections import defaultdict
from dataclasses import dataclass
from decimal import Decimal
from pathlib import Path
from typing import Any

from markov_collection.adapters.runtimes.ascend.clock import AscendClock
from markov_collection.adapters.runtimes.ascend.queues import NativeQueues, ServiceExecutions


def nanoseconds(microseconds: Any) -> int:
    return int(Decimal(str(microseconds)) * 1000)


@dataclass
class Interval:
    begin: int
    end: int
    thread: int
    record: dict[str, Any]

    @classmethod
    def read(cls, record: dict[str, Any], offset_ns: int) -> Interval:
        begin = nanoseconds(record["ts"]) - offset_ns
        return cls(begin, begin + nanoseconds(record["dur"]), int(record["tid"]), record)

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
class StreamWait:
    api: Interval
    stream: int
    scope: int
    begin: int = 0
    end: int = 0


@dataclass
class DevicePoint:
    host: Interval
    api: Interval
    task: DeviceTask
    stream: int

    @property
    def order(self) -> int:
        return self.task.end


class AscendTrace:
    host_waits = frozenset({
        "aclrtSynchronizeStream", "aclrtSynchronizeStreamWithTimeout", "aclrtSynchronizeEvent",
        "aclrtSynchronizeEventWithTimeout", "aclrtSynchronizeDeviceWithTimeout",
    })
    stream_waits = frozenset({"aclrtStreamWaitEvent", "aclrtStreamWaitEventWithTimeout"})

    def __init__(self, profile: Path, runtime: Path, native_points: set[int], services: list[dict], trace: Path, clock_samples: list[dict]) -> None:

        # Native service/API intervals use RAW; device time needs an independent counter calibration.
        start, = profile.glob("PROF_*/host/start_info")
        device, = profile.glob("PROF_*/device_*")
        host_offset = self.clock_offset(start)
        self.clock = AscendClock(device, clock_samples)
        rows = json.loads(trace.read_text(), parse_float=Decimal)
        flips = self.task_flips(profile)
        intervals = [self.device_interval(row) if "Task Type" in row.get("args", {}) else Interval.read(row, host_offset)
                     for row in rows if row.get("ph") == "X"]
        self.scopes = {}
        self.points = {}
        windows = []
        self.excluded = []
        enqueues, dequeues = [], []
        self.apis: dict[int, Interval] = {}
        self.tasks: dict[int, list[DeviceTask]] = defaultdict(list)
        self.payload = []
        processes = set()
        streams = defaultdict(list)
        for interval in intervals:
            row = interval.record
            name = row["name"]
            if name == "markov.window":
                windows.append(interval)
            elif name.startswith("markov.scope/"):
                identity = int(name.split("/")[1])
                if identity in self.scopes:
                    raise ValueError(f"duplicate semantic scope marker: {identity}")
                self.scopes[identity] = interval
                processes.add(row["pid"])
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
                self.payload.append(task)
        self.process, = processes
        if len(windows) != 1:
            raise ValueError("native recording requires one closed observation window")
        self.window, = windows
        for scope in services:
            identity = scope["id"]
            if identity in self.scopes:
                raise ValueError(f"service has two competing host observations: {identity}")
            interval = scope["host_interval"]
            self.scopes[identity] = Interval(
                interval["begin_raw_ns"], interval["end_raw_ns"],
                scope["thread"], {"name": f"markov.scope/{identity}"},
            )
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
            self.runtime.append(Interval(row["begin_raw_ns"], row["end_raw_ns"], row["thread"], row))
        queues = NativeQueues(self.runtime)
        self.services = ServiceExecutions(services, queues)
        for native in self.runtime:
            row = native.record
            if row["api"].startswith("aclrt") and row["stream_status"] == 0 and row["stream"] in self.services.streams:
                raise ValueError("a private transport queue has external runtime operations; explicit ports are required")
        self.services.assign(self.payload)

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
        self.order_stream_commands()
        self.device_points = {identity: self.device_point(identity) for identity in self.points}
        observed = {point.api.record["args"]["connection_id"] for identity, point in self.device_points.items() if identity not in native_points}

        # A native fence waits for submitted business work, not the observer's synthetic stream markers.
        submitted: dict[int, list[tuple[int, DeviceTask | StreamWait]]] = defaultdict(list)
        submitting = []
        groups = {}
        for command in self.commands:
            if isinstance(command, StreamWait):
                submitted[command.stream].append((command.api.end, command))
                submitting.append(Interval(command.api.begin, command.api.end, command.stream, {}))
                continue
            connection = command.record["args"]["connection_id"]
            if command.service is None and connection in self.apis and connection not in observed and not self.auxiliary(command) and not self.outside_window(command):
                key = command.stream, connection
                if key not in groups or command.end > groups[key].end:
                    groups[key] = command

        # SDK correlations may group several device tasks; driver submissions bound when the whole group is accepted.
        for (stream, connection), command in groups.items():
            begin, end = queues.window(self.apis[connection], stream)
            submitting.append(Interval(begin, end, stream, {}))
            submitted[stream].append((end, command))
        self.submitting = ThreadIntervals(submitting)
        self.frontiers = {}
        for stream, values in submitted.items():
            values.sort(key=lambda value: value[0])
            latest = []
            for _, task in values:
                latest.append(task if not latest or task.end > latest[-1].end else latest[-1])
            self.frontiers[stream] = [time for time, _ in values], latest

    @staticmethod
    def clock_offset(path: Path) -> int:
        clock = json.loads(path.read_text())
        return nanoseconds(clock["collectionTimeBegin"]) - int(clock["clockMonotonicRaw"])

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

    def order_stream_commands(self) -> None:
        self.waits: dict[int, StreamWait] = {}
        waits: dict[int, list[StreamWait]] = defaultdict(list)
        for native, _ in self.synchronizations:
            if native.record["api"] not in self.stream_waits:
                continue
            api = self.native_api(native)
            origin = self.origin(api)
            owner = self.owner(origin)
            if owner is None or self.excluded_ranges.enclosing(origin) is not None:
                continue
            if native.record["stream_status"] != 0:
                raise ValueError("stream wait has no observed native stream identity")
            wait = StreamWait(api, native.record["stream"], owner)
            self.waits[api.record["args"]["connection_id"]] = wait
            waits[wait.stream].append(wait)

        streams: dict[int, list[DeviceTask]] = defaultdict(list)
        for task in self.payload:
            if task.record["args"]["connection_id"] not in self.waits:
                streams[task.stream].append(task)
        self.commands: list[DeviceTask | StreamWait] = []
        for stream in sorted(streams.keys() | waits.keys()):
            tasks = sorted(streams[stream], key=lambda task: task.begin)
            positions = [task.begin for task in tasks]
            gaps: dict[int, list[StreamWait]] = defaultdict(list)
            for wait in waits[stream]:
                observed = self.tasks[wait.api.record["args"]["connection_id"]]
                if observed:
                    task, = observed
                    if task.stream != stream or task.record["args"]["Task Type"] != "EVENT_WAIT":
                        raise ValueError("stream wait differs from its SDK device task")
                    position = bisect_left(positions, task.begin)
                else:

                    # Submission order bounds the missing command; unknown or concurrent commands cannot be guessed past.
                    lower, upper = 0, len(tasks)
                    for index, task in enumerate(tasks):
                        api = self.apis.get(task.record["args"]["connection_id"])
                        if api is None:
                            continue
                        if api.end <= wait.api.begin:
                            lower = max(lower, index + 1)
                        elif api.begin >= wait.api.end:
                            upper = min(upper, index)
                    if lower != upper:
                        raise ValueError(f"stream {stream} wait in scope {wait.scope} has no unique command position: {lower}..{upper}")
                    position = lower
                gaps[position].append(wait)

            ordered = []
            for position in range(len(tasks) + 1):
                local = gaps[position]
                physical = [self.tasks[wait.api.record["args"]["connection_id"]] for wait in local]
                if all(physical):
                    local.sort(key=lambda wait: self.tasks[wait.api.record["args"]["connection_id"]][0].begin)
                else:
                    local.sort(key=lambda wait: wait.api.begin)
                    if any(left.api.end > right.api.begin for left, right in zip(local, local[1:])):
                        raise ValueError(f"concurrent stream waits have no established order on stream {stream}")
                ordered.extend(local)
                if position < len(tasks):
                    ordered.append(tasks[position])

            # The API defines a wait even when hardware elides its task; both use the same logical entry/exit coordinates.
            for index, command in enumerate(ordered):
                command.begin, command.end = 2 * index, 2 * index + 1
                if isinstance(command, StreamWait):
                    for task in self.tasks[command.api.record["args"]["connection_id"]]:
                        task.begin, task.end = command.begin, command.end
            self.commands.extend(ordered)
        self.payload = [command for command in self.commands if isinstance(command, DeviceTask)]

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

    def measurements(self, observation: dict) -> dict:
        spans, resources = observation["spans"], observation["resources"]
        positions = [set() for _ in resources]
        for span in spans:
            positions[span["resource"]].update((span["begin"], span["end"]))
        for anchor in self.anchors(observation):
            positions[spans[anchor["span"]]["resource"]].add(anchor["order"])

        clocks = {}
        for command in self.commands:
            if isinstance(command, DeviceTask):
                interval = self.device_interval(command.record)
            else:
                physical = self.tasks[command.api.record["args"]["connection_id"]]
                interval = self.device_interval(physical[0].record) if physical else None

            # An elided device wait has a proved order, but no measured device timestamp.
            clocks[command.stream, command.begin] = interval.begin if interval else None
            clocks[command.stream, command.end] = interval.end if interval else None

        coordinates = []
        for resource, orders in enumerate(positions):
            descriptor = resources[resource]
            stream = int(descriptor["identity"].split("/")[1]) if descriptor["kind"] == "stream" else None
            for order in sorted(orders):

                # Host/service coordinates are RAW timestamps; stream coordinates are strictly ordinal.
                measured = clocks[stream, order] if stream is not None else order
                if stream is not None and measured is not None and not self.clock.begin_ns <= measured <= self.clock.end_ns:
                    raise ValueError("observed device boundary lies outside the clock calibration window")
                coordinates.append({"resource": resource, "order": order, "time_ns": measured})
        return {
            "clock": "monotonic_raw_ns",
            "window_ns": [self.window.begin, self.window.end],
            "coordinates": coordinates,
        }

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

        # SDK clock conversion has finite precision: require one overlapping call of the same API and thread.
        calls = [native for native in self.runtime_ranges.overlapping(api) if native.record["api"] == api.record["args"]["id"]]
        if len(calls) > 1:
            raise ValueError(f"ambiguous native API instance: {api.record['args']['id']}")
        return calls[0] if calls else None

    def native_api(self, native: Interval) -> Interval:
        calls = [api for api in self.api_ranges.overlapping(native) if api.record["args"]["id"] == native.record["api"]]
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

    def submitted(self, before: int, stream: int | None) -> list[DeviceTask | StreamWait]:
        result = []
        if stream is not None and stream not in self.frontiers:

            # An observed stream may contain only markers; the preceding warmup was drained.
            return result
        selected = self.frontiers if stream is None else {stream: self.frontiers[stream]}
        for stream, (starts, latest) in selected.items():

            # Without per-SQE correspondence, an overlapping dispatch cannot identify the fence's exact frontier.
            if any(window.begin < before < window.end for window in self.submitting.overlapping(Interval(before, before, stream, {}))):
                raise ValueError(f"stream {stream} fence overlaps a native command-group submission")
            index = bisect_right(starts, before) - 1
            if index >= 0:
                result.append(latest[index])
        return result
