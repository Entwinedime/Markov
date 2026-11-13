"""Derive stream coordinates without changing the native evidence they refer to."""

from __future__ import annotations

from bisect import bisect_left, bisect_right
from collections import defaultdict
from dataclasses import dataclass

from markov_collection.adapters.runtimes.ascend.trace import AscendTrace, DeviceTask, Interval, ThreadIntervals


@dataclass
class NativeStreamCall:
    api: Interval
    stream: int
    scope: int
    native: Interval
    completion: Interval | None = None


@dataclass
class StreamCommand:
    source: DeviceTask | NativeStreamCall
    begin: int
    end: int

    @property
    def stream(self) -> int:
        return self.source.stream


class StreamCommands:
    def __init__(self, trace: AscendTrace) -> None:
        self.trace = trace
        self.commands: list[StreamCommand] = []
        self.stream_calls: dict[int, StreamCommand] = {}
        self.physical: dict[tuple[int, int], StreamCommand] = {}
        self.native_calls: list[NativeStreamCall] = []
        self.effects: dict[int, list[StreamCommand]] = {}
        self.order_commands()
        self.payload = [command for command in self.commands if isinstance(command.source, DeviceTask)]
        self.submission_frontiers()

    def task(self, task: DeviceTask) -> StreamCommand:
        return self.physical[task.stream, task.begin]

    def order_commands(self) -> None:
        trace = self.trace
        calls: dict[int, list[NativeStreamCall]] = defaultdict(list)
        connections = set()
        covered = {}
        for native in trace.runtime:
            name = native.record["api"]
            copy = name == "aclrtMemcpyAsync" and native.record["bytes"] > 0
            if name not in trace.stream_waits and not copy:
                continue
            if native.end <= trace.window.begin or trace.window.end <= native.begin:
                continue
            api = trace.native_api(native)
            connection = api.record["args"]["connection_id"]

            origin = trace.origin(api)
            owner = trace.owner(origin)
            if owner is None or owner in trace.closed_scopes or trace.excluded_ranges.enclosing(origin) is not None:
                continue
            if native.record["stream_status"] != 0:
                raise ValueError("native stream operation has no observed stream identity")
            call = NativeStreamCall(api, native.record["stream"], owner, native)
            self.native_calls.append(call)
            if copy:
                tasks = trace.tasks.get(connection) or self.covering_group(call)
                if tasks:
                    if any(task.stream != call.stream for task in tasks):
                        raise ValueError("native copy differs from its SDK command stream")
                    covered[connection] = tasks
                    continue

                call.completion = self.copy_completion(call)
                self.effects[connection] = []
                continue

            # Only actual queue operations receive stream coordinates; synchronous copies remain on their caller.
            calls[call.stream].append(call)
            connections.add(connection)

        streams: dict[int, list[DeviceTask]] = defaultdict(list)
        for task in trace.device_tasks:
            if task.record["args"]["connection_id"] not in connections:
                streams[task.stream].append(task)
        for stream in sorted(streams.keys() | calls.keys()):
            tasks = sorted(streams[stream], key=lambda task: task.begin)
            positions = [task.begin for task in tasks]
            gaps: dict[int, list[NativeStreamCall]] = defaultdict(list)
            for call in calls[stream]:
                observed = trace.tasks.get(call.api.record["args"]["connection_id"], [])
                if observed:
                    task, = observed
                    if task.stream != stream or task.record["args"]["Task Type"] != "EVENT_WAIT":
                        raise ValueError("stream wait differs from its SDK device task")
                    position = bisect_left(positions, task.begin)
                else:

                    # A missing SDK task is a logical wait only when the native API submitted no physical queue work.
                    if any(submission.record["tasks"] != 0 for submission in trace.queues.inside(call.native, stream)):
                        raise ValueError("unmatched stream wait has native queue submissions")

                    # Submission order bounds the missing command; concurrent commands cannot be guessed past.
                    lower, upper = 0, len(tasks)
                    for index, task in enumerate(tasks):
                        api = trace.apis.get(task.record["args"]["connection_id"])
                        if api is None:
                            continue
                        if api.end <= call.api.begin:
                            lower = max(lower, index + 1)
                        elif api.begin >= call.api.end:
                            upper = min(upper, index)
                    if lower != upper:
                        raise ValueError(f"stream {stream} operation in scope {call.scope} has no unique command position: {lower}..{upper}")
                    position = lower
                gaps[position].append(call)

            ordered = []
            for position in range(len(tasks) + 1):
                local = gaps[position]
                if all(trace.tasks.get(call.api.record["args"]["connection_id"]) for call in local):
                    local.sort(key=lambda call: trace.tasks[call.api.record["args"]["connection_id"]][0].begin)
                else:
                    local.sort(key=lambda call: call.api.begin)
                    if any(left.api.end > right.api.begin for left, right in zip(local, local[1:])):
                        raise ValueError(f"concurrent native stream operations have no established order on stream {stream}")
                ordered.extend(local)
                if position < len(tasks):
                    ordered.append(tasks[position])

            # Derived ordinals never overwrite SDK task positions or independently measured timestamps.
            for index, source in enumerate(ordered):
                command = StreamCommand(source, 2 * index, 2 * index + 1)
                self.commands.append(command)
                physical = [source] if isinstance(source, DeviceTask) else trace.tasks.get(source.api.record["args"]["connection_id"], [])
                for task in physical:
                    self.physical[task.stream, task.begin] = command
                if isinstance(source, NativeStreamCall):
                    connection = source.api.record["args"]["connection_id"]
                    self.stream_calls[connection] = command
                    self.effects[connection] = [command]

        for connection, tasks in covered.items():
            self.effects[connection] = [self.task(task) for task in tasks]

    def copy_completion(self, source: NativeStreamCall) -> Interval:
        row = source.native.record
        copies = [call for call in self.trace.runtime_ranges.inside(source.native)
                  if call.record["api"] in {"drvMemcpy", "halMemcpy"}
                  and all(call.record[key] == row[key] for key in ("source", "destination", "bytes"))]
        if not copies:
            raise ValueError(f"copy in scope {source.scope} "
                             "has neither correlated device work nor an observed synchronous transfer of its buffers")

        # Staging copies have different addresses. A successful synchronous copy of the full range proves visibility.
        return max(copies, key=lambda call: call.end)

    def covering_group(self, call: NativeStreamCall) -> list[DeviceTask]:
        trace = self.trace
        submissions = trace.queues.inside(call.native, call.stream)
        if not submissions:
            return []
        groups = []
        for api in trace.api_ranges.overlapping(call.api):
            tasks = trace.tasks.get(api.record["args"]["connection_id"], [])
            if not tasks or not api.contains(call.api) or trace.owner(trace.origin(api)) != call.scope:
                continue
            if all(task.stream == call.stream for task in tasks) and trace.queues.inside(api, call.stream) == submissions:
                groups.append(tasks)
        if len(groups) > 1:
            raise ValueError(f"native copy in scope {call.scope} has competing SDK command groups")

        # The same driver submissions observed through nested APIs represent one group, not extra work.
        return groups[0] if groups else []

    def submission_frontiers(self) -> None:
        trace = self.trace
        submitted: dict[int, list[tuple[int, StreamCommand]]] = defaultdict(list)
        submitting = []
        groups = {}
        for command in self.commands:
            source = command.source
            if isinstance(source, NativeStreamCall):
                physical = trace.tasks.get(source.api.record["args"]["connection_id"])
                begin, end = trace.queues.window(source.api, command.stream) if physical else (source.native.begin, source.native.end)
                submitted[command.stream].append((end, command))
                submitting.append(Interval(begin, end, command.stream, {}))
                continue
            connection = source.record["args"]["connection_id"]
            if source.service is None and connection in trace.apis and connection not in trace.observer_connections and not trace.auxiliary(source) and not trace.outside_window(source):
                key = command.stream, connection
                if key not in groups or command.end > groups[key].end:
                    groups[key] = command

        # SDK correlations can group several tasks; driver evidence bounds acceptance of the entire group.
        for (stream, connection), command in groups.items():
            begin, end = trace.queues.window(trace.apis[connection], stream)
            submitting.append(Interval(begin, end, stream, {}))
            submitted[stream].append((end, command))
        self.submitting = ThreadIntervals(submitting)
        self.frontiers = {}
        for stream, values in submitted.items():
            values.sort(key=lambda value: value[0])
            latest = []
            for _, command in values:
                latest.append(command if not latest or command.end > latest[-1].end else latest[-1])
            self.frontiers[stream] = [time for time, _ in values], latest

    def submitted(self, before: int, stream: int | None) -> list[StreamCommand]:
        result = []
        if stream is not None and stream not in self.frontiers:

            # Marker-only streams contain no business work after the drained warmup.
            return result
        selected = self.frontiers if stream is None else {stream: self.frontiers[stream]}
        for stream, (starts, latest) in selected.items():
            if any(window.begin < before < window.end for window in self.submitting.overlapping(Interval(before, before, stream, {}))):
                raise ValueError(f"stream {stream} fence overlaps a native command-group submission")
            index = bisect_right(starts, before) - 1
            if index >= 0:
                result.append(latest[index])
        return result

    def evidence(self, resources: dict) -> list[dict]:
        records = []
        for call in self.native_calls:
            connection = call.api.record["args"]["connection_id"]
            commands = []
            for command in self.effects[connection]:
                source = command.source
                physical = [source] if isinstance(source, DeviceTask) else self.trace.tasks.get(source.api.record["args"]["connection_id"], [])
                task = physical[0].record["args"] if physical else None
                commands.append({
                    "begin": command.begin, "end": command.end,
                    "sdk": {name: task[name] for name in ("connection_id", "Batch Id", "Task Id", "Subtask Id")} if task else None,
                })
            records.append({
                "thread": call.native.thread, "begin_counter": call.native.begin, "connection": connection,
                "source_scope": call.scope,
                "resource": resources["thread", call.completion.thread] if call.completion else resources["stream", call.stream],
                "commands": commands,
                "completion": {key: call.completion.record[key] for key in ("api", "thread", "begin_counter")}
                              if call.completion else None,
            })
        return records

    def measurements(self, observation: dict) -> dict:
        trace = self.trace
        spans, resources = observation["spans"], observation["resources"]
        positions = [set() for _ in resources]
        for span in spans:
            positions[span["resource"]].update((span["begin"], span["end"]))
        for anchor in trace.anchors(observation):
            positions[spans[anchor["span"]]["resource"]].add(anchor["order"])

        clocks = {}
        for command in self.commands:
            source = command.source
            physical = [source] if isinstance(source, DeviceTask) else trace.tasks.get(source.api.record["args"]["connection_id"], [])
            interval = trace.device_interval(physical[0].record) if physical else None

            # Only real SDK tasks provide device timestamps. Unmeasured waits remain pure boundaries.
            clocks[command.stream, command.begin] = interval.begin if interval else None
            clocks[command.stream, command.end] = interval.end if interval else None

        coordinates = []
        for resource, orders in enumerate(positions):
            descriptor = resources[resource]
            stream = int(descriptor["identity"].split("/")[1]) if descriptor["kind"] == "stream" else None
            for order in sorted(orders):

                # Every retained host cut names an observed native or SDK endpoint; missing handoff is an error.
                measured = clocks[stream, order] if stream is not None else trace.host_times[descriptor["identity"], order]
                if stream is not None and measured is not None and not trace.clock.begin_ns <= measured <= trace.clock.end_ns:
                    raise ValueError("observed device boundary lies outside the clock calibration window")
                coordinates.append({"resource": resource, "order": order, "time_ns": measured})
        return {
            "clock": "monotonic_raw_ns",
            "window_ns": [trace.host_times[f"thread/{trace.window.thread}", side] for side in (trace.window.begin, trace.window.end)],
            "coordinates": coordinates,
        }
