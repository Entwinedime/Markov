"""Associate device work and runtime effects through native queues and host scopes."""

from __future__ import annotations

import json
from bisect import bisect_right
from collections import defaultdict
from collections.abc import Iterator
from pathlib import Path
from typing import Any

from markov_collection.adapters.runtimes.ascend.native_calls import NativeRuntimeCalls, event_time_ns


class NativeQueues:
    def __init__(self, events: list[dict[str, Any]]) -> None:
        self.events = events
        timestamps = [event_time_ns(event) for event in events]
        completions = [
            start + (event_time_ns(event, "dur") if "dur" in event else 0)
            for event, start in zip(events, timestamps, strict=True)
        ]

        # Preserve relative order with compact indices; timestamps remain source evidence only.
        positions = {timestamp: index for index, timestamp in enumerate(sorted(set(timestamps + completions)))}
        self.order = [positions[timestamp] for timestamp in timestamps]
        self.completion_order = [positions[timestamp] for timestamp in completions]

        self.enqueues: dict[tuple[int, int], int] = {}
        self.dequeues: dict[int, list[int]] = defaultdict(list)
        for index, event in enumerate(events):
            if event.get("cat") == "enqueue":
                key = (event["pid"], event["args"]["correlation_id"])
                if key in self.enqueues:
                    raise ValueError("native queue correlation has multiple enqueue records")
                self.enqueues[key] = index
            elif event.get("cat") == "dequeue":
                self.dequeues[event["tid"]].append(index)

        for queue in self.dequeues.values():
            queue.sort(key=lambda index: event_time_ns(events[index]))
        self.positions = {
            thread: [event_time_ns(events[index]) for index in queue] for thread, queue in self.dequeues.items()
        }

    def item(self, event: dict[str, Any]) -> tuple[int, int] | None:
        thread = event.get("args", {}).get("Thread Id", event["tid"])
        start = event_time_ns(event)
        position = bisect_right(self.positions.get(thread, []), start) - 1
        if position < 0:
            return None

        identity = self.dequeues[thread][position]
        dequeue = self.events[identity]

        # Containment identifies the queue item; a nearby dequeue is not an association.
        if start + event_time_ns(event, "dur") > event_time_ns(dequeue) + event_time_ns(dequeue, "dur"):
            return None

        enqueue = self.enqueues.get((dequeue["pid"], dequeue["args"]["correlation_id"]))
        return None if enqueue is None else (enqueue, identity)

    def origin(self, event: dict[str, Any]) -> dict[str, Any] | None:
        item = self.item(event)
        return None if item is None else self.events[item[0]]

    def submission(self, identity: int) -> dict[str, Any]:
        event = self.events[identity]
        item = self.item(event)
        queue = None
        if item is not None:
            enqueue = self.events[item[0]]
            queue = {
                "process": enqueue["pid"],
                "correlation": enqueue["args"]["correlation_id"],
                "producer_thread": enqueue["tid"],
                "enqueue_event": item[0],
                "dequeue_event": item[1],
                "enqueue_order": self.order[item[0]],
                "dequeue_order": self.order[item[1]],
                "dequeue_completion_order": self.completion_order[item[1]],
            }

        return {
            "source_event": identity,
            "order": self.order[identity],
            "completion_order": self.completion_order[identity],
            "native_entry": event["name"],
            "thread": event.get("args", {}).get("Thread Id", event["tid"]),
            "queue": queue,
        }


class AscendTrace:
    runtime_kinds = {
        "aclrtMemcpy": "copy",
        "aclrtMemcpyAsync": "copy_async",
        "aclrtSynchronizeStream": "wait_stream",
        "aclrtSynchronizeStreamWithTimeout": "wait_stream",
        "aclrtSynchronizeDeviceWithTimeout": "wait_device",
        "aclrtRecordEvent": "record_event",
        "aclrtStreamWaitEvent": "wait_event",
        "aclrtSynchronizeEvent": "wait_event_complete",
        "aclrtSynchronizeEventWithTimeout": "wait_event_complete",
    }

    def __init__(self, path: Path, runtime_directory: Path | None = None, *, device: int) -> None:
        self.device = device
        self.runtime = (
            NativeRuntimeCalls(runtime_directory, path.parent.parent) if runtime_directory is not None else None
        )

        categories = {"async_npu", "async_task_queue", "enqueue", "dequeue", "HostToDevice"}
        self.events = []
        for event in self.read_events(path):
            arguments = event.get("args", {})
            task = "Task Type" in arguments
            launch = "connection_id" in arguments and (
                event.get("name") == "Node@launch" or event.get("name", "").startswith("AscendCL@")
            )
            runtime = self.runtime_api(event) is not None
            if (
                event.get("cat") not in categories
                and not task
                and not launch
                and not runtime
                and not event.get("name", "").startswith("markov.")
            ):
                continue

            projected = {
                key: event[key] for key in ("name", "pid", "tid", "ts", "dur", "ph", "cat", "id", "bp") if key in event
            }
            if task or launch or runtime or event.get("cat") in ("enqueue", "dequeue"):
                projected["args"] = arguments
            self.events.append(projected)

        # Retain native API entries needed by device connections, rather than every ACL_NN call.
        connections = {
            event["args"]["connection_id"]
            for event in self.events
            if "Task Type" in event.get("args", {}) and "connection_id" in event["args"]
        }
        self.events = [
            event
            for event in self.events
            if not event.get("name", "").startswith("AscendCL@")
            or self.runtime_api(event) is not None
            or event["args"].get("connection_id") in connections
        ]
        self.queues = NativeQueues(self.events)

    @staticmethod
    def runtime_api(event: dict[str, Any]) -> str | None:
        if not event.get("name", "").startswith("AscendCL@"):
            return None
        name = event.get("args", {}).get("id", "")
        if name.startswith(("aclrtMemcpy", "aclrtSynchronize", "aclrtRecordEvent", "aclrtStreamWaitEvent")):
            return name
        return None

    @staticmethod
    def read_events(path: Path) -> Iterator[dict[str, Any]]:
        decoder = json.JSONDecoder()
        with path.open() as stream:
            buffer = stream.read(131072)
            position = len(buffer) - len(buffer.lstrip())
            if buffer[position : position + 1] != "[":
                raise ValueError("Ascend trace_view must contain an event array")

            position += 1
            first = True
            while True:
                while position < len(buffer) and buffer[position].isspace():
                    position += 1

                if position == len(buffer):
                    buffer = stream.read(131072)
                    position = 0
                    if not buffer:
                        raise ValueError("incomplete Ascend event array")
                    continue

                if buffer[position] == "]":
                    if (buffer[position + 1 :] + stream.read()).strip():
                        raise ValueError("unexpected content after Ascend event array")
                    return

                if not first:
                    if buffer[position] != ",":
                        raise ValueError("missing Ascend event separator")
                    position += 1

                # Keep only the incomplete event when a record crosses a read boundary.
                while True:
                    while position < len(buffer) and buffer[position].isspace():
                        position += 1
                    try:
                        event, position = decoder.raw_decode(buffer, position)
                        break
                    except json.JSONDecodeError:
                        more = stream.read(131072)
                        if not more:
                            raise
                        buffer = buffer[position:] + more
                        position = 0

                yield event
                first = False
                buffer = buffer[position:]
                position = 0

    def launch_sources(self) -> dict[int, int]:
        launches = defaultdict(list)
        for identity, event in enumerate(self.events):
            if "connection_id" in event.get("args", {}) and (
                event.get("name") == "Node@launch" or event.get("name", "").startswith("AscendCL@")
            ):
                launches[event["args"]["connection_id"]].append(identity)

        sources = {}
        for connection, group in launches.items():
            direct = [identity for identity in group if self.events[identity]["name"] == "Node@launch"]
            candidates = direct or group
            if len(candidates) == 1:
                sources[connection] = candidates[0]

        return sources

    def ownership(self, queries: dict[int, dict[str, Any]]) -> dict[int, dict[str, Any]]:
        scopes = defaultdict(list)
        for event in self.events:
            if event.get("ph") == "X" and event.get("name", "").startswith("markov."):
                scopes[event["tid"]].append(event)

        grouped = defaultdict(list)
        for identity, event in queries.items():
            thread = event.get("args", {}).get("Thread Id", event["tid"])
            grouped[thread].append((identity, event))

        result = {}
        for thread, group in grouped.items():
            intervals = sorted(
                (
                    (event_time_ns(scope), event_time_ns(scope) + event_time_ns(scope, "dur"), scope)
                    for scope in scopes[thread]
                ),
                key=lambda interval: interval[0],
            )

            active = []
            position = 0
            for identity, event in sorted(group, key=lambda item: event_time_ns(item[1])):
                timestamp = event_time_ns(event)
                end = timestamp + (event_time_ns(event, "dur") if "dur" in event else 0)
                while position < len(intervals) and intervals[position][0] <= timestamp:
                    active.append(intervals[position])
                    position += 1
                active = [interval for interval in active if interval[1] >= timestamp]
                owners = [interval[2] for interval in active if interval[1] >= end]

                # CANN uses a synthetic pid; its native Thread Id refers to the host marker thread.
                result[identity] = {
                    "operation": self.innermost(owners, "markov.operation/"),
                    "region": self.innermost(owners, "markov.region/"),
                    "baseline": self.innermost(owners, "markov.baseline/"),
                    "observer": any(scope["name"] == "markov.observer" for scope in owners),
                    "auxiliary": any(scope["name"] == "markov.auxiliary" for scope in owners),
                }

        return result

    def physical_work(self, region_count: int, operation_count: int) -> dict[str, Any]:
        tasks: dict[tuple[Any, Any, int], list[tuple[int, dict[str, Any]]]] = defaultdict(list)
        records = {}
        starts = {}
        finishes = {}
        observer_controls = []

        for index, event in enumerate(self.events):
            key = (event.get("pid"), event.get("tid"))
            if event.get("ph") == "X" and "Task Type" in event.get("args", {}):
                if event["args"]["Task Type"] in ("PROFILING_ENABLE", "PROFILING_DISABLE"):
                    observer_controls.append({"source_event": index, "name": event["name"]})
                    continue
                tasks[(*key, event_time_ns(event))].append((index, event))
                records[index] = event

            if event.get("cat") == "async_npu":
                if event["ph"] == "s":
                    starts[event["id"]] = event
                elif event["ph"] == "f":
                    finishes[event["id"]] = event

        # Prefer explicit profiler flows, then native launch connections for the remaining device tasks.
        claims = {}
        for identity, finish in finishes.items():
            start = starts.get(identity)
            if start is not None:
                endpoint = (finish["pid"], finish["tid"], event_time_ns(finish))
                for index, _ in tasks.get(endpoint, []):
                    claims[index] = (start, "torch_to_npu_flow")

        launches = self.launch_sources()
        for index, task in records.items():
            if index not in claims and task["args"].get("connection_id") in launches:
                event = self.events[launches[task["args"]["connection_id"]]]
                origin = self.queues.origin(event)
                claims[index] = (origin if origin is not None else event, "native_connection")

        queries = {index: event for index, (event, _) in claims.items()}
        actions = []
        for index, event in enumerate(self.events):
            api = self.runtime_api(event)
            if api is None:
                continue

            submission = self.queues.submission(index)
            queue = submission["queue"]
            queries[index] = self.events[queue["enqueue_event"]] if queue is not None else event
            actions.append(
                {
                    "source_event": index,
                    "kind": self.runtime_kinds.get(api, "unclassified"),
                    "native_api": api,
                    "context": "runtime_queue" if queue is not None else "host",
                    "submission": submission,
                    "native_call": self.runtime.arguments(event, event_time_ns(event))
                    if self.runtime is not None
                    else None,
                }
            )

        if self.runtime is not None:
            for index, event in enumerate(self.events):
                if event.get("name") in self.runtime.callbacks:
                    queries[index] = self.queues.origin(event) or event

        owners = self.ownership(queries)
        if self.runtime is not None:
            self.runtime.bind_callbacks(self.events, actions, owners)

        result = []
        for index, (_, association) in claims.items():
            task = records[index]
            launch = launches.get(task["args"].get("connection_id"))
            result.append(
                {
                    "name": task["name"],
                    "kind": "transfer" if task["args"]["Task Type"] == "SDMA_SQE" else "device",
                    "stream": task["args"]["Physic Stream Id"],
                    "task_type": task["args"]["Task Type"],
                    "duration_ns": event_time_ns(task, "dur"),
                    **owners[index],
                    "source_event": index,
                    "association": association,
                    "order": self.queues.order[index],
                    "submission": self.queues.submission(launch) if launch is not None else None,
                }
            )

        for action in actions:
            action.update(owners[action["source_event"]])
            parameters = action["native_call"]
            target = None
            if parameters is not None:
                if (
                    action["kind"] in ("copy_async", "wait_stream", "record_event", "wait_event")
                    and parameters["stream_status"] == 0
                ):
                    target = {"kind": "stream", "identity": parameters["stream"]}
                elif action["kind"] in ("copy", "wait_device") and parameters["device_status"] == 0:
                    target = {"kind": "device", "identity": parameters["device"]}
                elif action["kind"] == "wait_event_complete" and "event" in parameters:
                    target = {"kind": "event", "identity": parameters["event"]}
            if action["kind"] == "wait_device":

                # Each source worker is restricted to its declared single physical device.
                if target is not None and target["identity"] != self.device:
                    raise ValueError("device wait disagrees with the source device declaration")
                target = {"kind": "device", "identity": self.device}
            action["target"] = target

        limits = {"region": region_count, "operation": operation_count}
        observed = {
            identity
            for identity, owner in owners.items()
            if all(owner[kind] is None or owner[kind] < count for kind, count in limits.items())
        }

        # The profiler can stop after the source flush; later scopes belong to the next observation window.
        outside = {
            "device_tasks": sum(task["source_event"] not in observed for task in result),
            "runtime_actions": sum(action["source_event"] not in observed for action in actions),
        }
        result = [task for task in result if task["source_event"] in observed]
        actions = [action for action in actions if action["source_event"] in observed]
        self.bind_transfers(result, actions)

        # Queue-drain dependencies need both the API's stream and the worker serving that stream.
        queues_by_stream: dict[int, set[int]] = defaultdict(set)
        for task in result:
            submission = task["submission"]
            if submission is not None and submission["queue"] is not None:
                queues_by_stream[task["stream"]].add(submission["thread"])
        for action in actions:
            if action["kind"] == "copy_async" and action["target"] is not None and action["context"] == "runtime_queue":
                queues_by_stream[action["target"]["identity"]].add(action["submission"]["thread"])

        streams = {task["stream"] for task in result}
        streams.update(
            action["target"]["identity"]
            for action in actions
            if action["target"] is not None and action["target"]["kind"] == "stream"
        )
        for action in actions:
            action["wait_frontiers"] = None
            parameters = action["native_call"]
            if action["kind"] not in ("wait_stream", "wait_device") or action["target"] is None:
                continue

            frontiers = {
                "queues": [],
                "streams": sorted(streams) if action["kind"] == "wait_device" else [action["target"]["identity"]],
            }

            # NPUStream's accessor drains the queue serving this stream; native queue items identify its worker.
            if (
                action["kind"] == "wait_stream"
                and action["context"] == "host"
                and parameters is not None
                and parameters.get("queue_scope") is not None
                and action["target"] is not None
            ):
                threads = queues_by_stream[parameters["stream"]]
                if len(threads) == 1:
                    frontiers["queues"] = list(threads)
            elif (
                action["kind"] == "wait_device"
                and action["context"] == "host"
                and parameters is not None
                and parameters.get("queue_scope") == "device"
            ):
                frontiers["queues"] = sorted({thread for threads in queues_by_stream.values() for thread in threads})
            action["wait_frontiers"] = frontiers

        host_threads = {
            event["tid"]
            for event in self.events
            if event.get("name", "").startswith(("markov.region/", "markov.operation/", "markov.baseline/"))
        }
        unassociated_runtime = [
            action["source_event"]
            for action in actions
            if action["context"] == "host"
            and self.events[action["source_event"]]["args"]["Thread Id"] not in host_threads
        ]
        unmatched = [
            {"source_event": index, "name": task["name"]} for index, task in records.items() if index not in claims
        ]

        host_scopes = []
        for identity, event in enumerate(self.events):
            if event.get("ph") != "X" or not event.get("name", "").startswith(("markov.operation/", "markov.region/")):
                continue
            kind, scope = event["name"].split("/", 1)
            kind, scope = kind.removeprefix("markov."), int(scope)
            if scope >= limits[kind]:
                continue
            host_scopes.append(
                {
                    "kind": kind,
                    "identity": scope,
                    "thread": event["tid"],
                    "source_event": identity,
                    "order": self.queues.order[identity],
                    "completion_order": self.queues.completion_order[identity],
                }
            )

        return {
            "snapshot": {**limits, "outside": outside},
            "work": result,
            "runtime_actions": actions,
            "host_scopes": host_scopes,
            "unassociated_runtime_actions": unassociated_runtime,
            "unassociated_tasks": unmatched,
            "observer_controls": observer_controls,
        }

    @staticmethod
    def bind_transfers(work: list[dict[str, Any]], actions: list[dict[str, Any]]) -> None:
        copies: dict[tuple[Any, ...], list[dict[str, Any]]] = defaultdict(list)
        for action in actions:
            if action["kind"] != "copy_async" or action["target"] is None:
                continue
            submission = action["submission"]
            queue = submission["queue"]
            key = (
                submission["thread"],
                None if queue is None else (queue["process"], queue["correlation"]),
                action["target"]["identity"],
            )
            copies[key].append(action)

        # A parent ACLNN launch can own the DMA connection; its nested memcpy API is the actual submitter.
        for task in work:
            submission = task["submission"]
            if task["kind"] != "transfer" or submission is None:
                continue
            queue = submission["queue"]
            key = (
                submission["thread"],
                None if queue is None else (queue["process"], queue["correlation"]),
                task["stream"],
            )
            candidates = []
            for action in copies.get(key, []):
                copy = action["submission"]
                if (
                    submission["order"] <= copy["order"] <= copy["completion_order"] <= submission["completion_order"]
                    or copy["order"]
                    <= submission["order"]
                    <= submission["completion_order"]
                    <= copy["completion_order"]
                ):
                    candidates.append(copy)
            if len(candidates) == 1:
                task["submission"] = candidates[0]

    @staticmethod
    def innermost(scopes: list[dict[str, Any]], prefix: str) -> int | None:
        candidates = [scope for scope in scopes if scope["name"].startswith(prefix)]
        if not candidates:
            return None

        scope = min(candidates, key=lambda item: event_time_ns(item, "dur"))
        return int(scope["name"].removeprefix(prefix))
