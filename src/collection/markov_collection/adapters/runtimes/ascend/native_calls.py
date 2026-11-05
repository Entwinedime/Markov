"""Join native API arguments through their own thread, entry point and clock mapping."""

from __future__ import annotations

import json
from bisect import bisect_right
from collections import Counter, defaultdict
from collections.abc import Sequence
from decimal import Decimal
from itertools import accumulate, pairwise
from operator import itemgetter
from pathlib import Path
from typing import Any

from markov_collection.adapters.runtimes.ascend.transfer import MemfabricObservation


def event_time_ns(event: dict[str, Any], field: str = "ts") -> int:
    return int(Decimal(str(event[field])) * 1000)


class NativeRuntimeCalls:
    def __init__(self, directory: Path, profiler: Path, process: int) -> None:
        if not directory.is_dir():
            raise NotADirectoryError(directory)

        clocks = list(profiler.glob("PROF*/host/start_info"))
        if len(clocks) != 1:
            raise ValueError("native runtime arguments need one CANN host clock mapping")

        clock = json.loads(clocks[0].read_text())
        self.offset_ns = int(Decimal(str(clock["collectionTimeBegin"])) * 1000) - int(clock["clockMonotonicRaw"])

        self.calls: dict[tuple[int, str], list[dict[str, Any]]] = defaultdict(list)
        for path in directory.glob(f"{process}.jsonl"):
            for index, line in enumerate(path.read_text().splitlines()):
                call = json.loads(line)
                call["source"] = {"process": int(path.stem), "record": index}
                self.calls[(call["thread"], call["api"])].append(call)

        self.prefix_ends = self.index_intervals(self.calls)

        self.native_services: dict[int, dict[str, Any]] = {}
        self.service_calls: Counter[str] = Counter()
        for path in directory.glob(f"{process}-memfabric.json"):
            recording = json.loads(path.read_text())
            for identity, allocation in enumerate(recording["allocations"]):

                # The SDK owns this private queue until shutdown; its tasks need no guessed per-call owner.
                self.native_services[allocation["stream"]] = {
                    "work": dict(MemfabricObservation.service), "stream": allocation["stream"],
                    "process": process, "allocation": identity,
                }
            self.service_calls.update(
                "baseline" if call["baseline"] is not None else "source"
                for call in recording["copies"] if call["status"] == 0
            )

        self.callbacks: dict[str, dict[str, Any]] = {}
        for path in directory.glob(f"{process}-callbacks.json"):
            for identity, call in enumerate(json.loads(path.read_text())):
                call["source"] = {"process": process, "callback": identity}
                call["queue_scope"] = self.queue_scope(call)

                # This torch_npu caller emits the device callback only after ACL_SUCCESS; stream callbacks precede it.
                if call["kind"] == "device" and any(
                    name.startswith("c10_npu::npuSynchronizeDevice(") for name in call["callers"]
                ):
                    call["result"] = 0
                call["kind"] = {
                    "stream": "wait_stream",
                    "device": "wait_device",
                    "record_event": "record_event",
                    "wait_event": "wait_event",
                    "synchronize_event": "wait_event_complete",
                }[call["kind"]]
                self.callbacks[f"markov.runtime/{process}/{identity}"] = call

        self.callback_kinds = {call["kind"] for call in self.callbacks.values()}

    @staticmethod
    def index_intervals(groups: dict[Any, list[dict[str, Any]]]) -> dict[Any, list[int]]:
        ends = {}
        for key, calls in groups.items():
            if any(call["end_raw_ns"] < call["begin_raw_ns"] for call in calls):
                raise ValueError("native work ends before it begins")
            calls.sort(key=itemgetter("begin_raw_ns"))
            ends[key] = list(accumulate((call["end_raw_ns"] for call in calls), max))
        return ends

    @staticmethod
    def enclosing_call(
        calls: Sequence[dict[str, Any]], prefix_ends: list[int], begin_ns: int, end_ns: int,
    ) -> dict[str, Any] | None:
        position = bisect_right(calls, begin_ns, key=itemgetter("begin_raw_ns"))
        match = None

        # Complete containment and uniqueness are required, including nested native calls.
        while position and prefix_ends[position - 1] >= end_ns:
            position -= 1
            call = calls[position]
            if call["end_raw_ns"] >= end_ns:
                if match is not None:
                    return None
                match = call
        return match

    @staticmethod
    def queue_scope(call: dict[str, Any]) -> str | None:
        if call["kind"] == "device":
            drained = any(
                name.startswith(("c10_npu::npuSynchronizeDevice(", "c10_npu::npuSynchronizeUsedDevices("))
                for name in call["callers"]
            )
            return "device" if drained else None
        if call["kind"] != "stream":
            return None

        entry_points = (
            ("c10_npu::acl::AclrtSynchronizeStreamWithTimeout(", 1),
            ("c10_npu::stream_synchronize(", 1),
            ("at_npu::native::copy_between_host_and_device_opapi(", 0),
        )

        # Only the immediate native caller proves the stream conversion; an outer copy may contain another wait.
        accessors = (
            "at_npu::native::copy_between_host_and_device_opapi(",
            "at_npu::native::copy_between_host_and_device(",
            "at_npu::native::process_unregistered_mem_location_type(",
            "at_npu::native::NPUNativeFunctions::_local_scalar_dense(",
            "c10_npu::NPUStream::synchronize(",
        )
        callers = call["callers"]
        for index, name in enumerate(callers):
            for prefix, offset in entry_points:
                if name.startswith(prefix):
                    position = index + offset
                    caller = callers[position] if position < len(callers) else ""
                    return call["queue_scope"] if caller.startswith(accessors) else None

        return None

    def arguments(self, event: dict[str, Any], timestamp_ns: int) -> dict[str, Any] | None:
        arguments = event["args"]
        instant = timestamp_ns - self.offset_ns
        key = arguments["Thread Id"], arguments["id"]
        calls = self.calls.get(key, ())
        match = self.enclosing_call(calls, self.prefix_ends.get(key, []), instant, instant + 1)
        return None if match is None else self.parameters(match)

    @staticmethod
    def parameters(call: dict[str, Any]) -> dict[str, Any]:
        return {
            name: call[name]
            for name in (
                "source",
                "device",
                "device_status",
                "stream",
                "stream_status",
                "bytes",
                "direction",
                "result",
                "event",
            )
            if name in call
        }

    @staticmethod
    def scope_key(thread: int, kind: str, owner: dict[str, Any]) -> tuple[Any, ...]:
        return (thread, kind, *(owner[name] for name in ("operation", "region", "baseline", "observer", "auxiliary")))

    @staticmethod
    def callback_origin(action: dict[str, Any]) -> int:

        # Event callbacks follow LaunchRecord/WaitEventTask on its producer, even when the API runs on a worker.
        queue = action["submission"]["queue"]
        if action["kind"] in ("record_event", "wait_event") and queue is not None:
            return queue["enqueue_event"]
        return action["source_event"]

    def bind_callbacks(
        self, events: list[dict[str, Any]], actions: list[dict[str, Any]], owners: dict[int, dict[str, Any]]
    ) -> list[tuple[int, int, int]]:
        bound = []
        markers: dict[tuple[Any, ...], list[int]] = defaultdict(list)
        calls: dict[tuple[Any, ...], list[dict[str, Any]]] = defaultdict(list)
        for identity, event in enumerate(events):
            callback = self.callbacks.get(event.get("name"))
            if callback is not None:
                key = self.scope_key(event["tid"], callback["kind"], owners[identity])
                markers[key].append(identity)

        for action in actions:
            if action["kind"] in self.callback_kinds:
                identity = self.callback_origin(action)
                event = events[identity]
                thread = event.get("args", {}).get("Thread Id", event["tid"])
                key = self.scope_key(thread, action["kind"], owners[action["source_event"]])
                calls[key].append(action)

        for key, identities in markers.items():
            actions_in_scope = sorted(
                calls[key], key=lambda action: event_time_ns(events[self.callback_origin(action)])
            )
            identities.sort(key=lambda identity: event_time_ns(events[identity]))
            if key[1] == "wait_device":

                # Device callbacks follow their API; a missing callback must not erase earlier completed pairs.
                ordered = [(identity, None) for identity in identities]
                ordered.extend((self.callback_origin(action), action) for action in actions_in_scope)
                ordered.sort(key=lambda item: event_time_ns(events[item[0]]))
                pairs = [
                    (identity, action)
                    for (_, action), (identity, next_action) in pairwise(ordered)
                    if action is not None and next_action is None
                ]
                ordered = [events[identity] for identity, _ in ordered]
            else:
                if len(identities) != len(actions_in_scope):
                    continue
                pairs = list(zip(identities, actions_in_scope, strict=True))
                ordered = []
                for identity, action in pairs:
                    ordered.extend(sorted([events[identity], events[self.callback_origin(action)]], key=event_time_ns))

            # One callback belongs to each call, in thread order; overlapping observations stay unbound.
            if any(
                event_time_ns(left) + event_time_ns(left, "dur") > event_time_ns(right)
                for left, right in pairwise(ordered)
            ):
                continue

            for identity, action in pairs:
                callback = self.callbacks[events[identity]["name"]]
                parameters = self.parameters(callback)
                direct = action["native_call"]
                if direct is not None:
                    if parameters["result"] is not None and direct["result"] != parameters["result"]:
                        raise ValueError("native synchronization callback disagrees with the API result")
                    for resource in ("device", "stream"):
                        status = f"{resource}_status"
                        if direct[status] == parameters[status] == 0 and direct[resource] != parameters[resource]:
                            raise ValueError("native synchronization callback disagrees with the API arguments")
                    parameters = {**parameters, **direct, "source": {**parameters["source"], **direct["source"]}}

                parameters["queue_scope"] = callback.get("queue_scope")
                action["native_call"] = parameters
                operation = callback.get("operation")
                if operation is not None:
                    bound.append((operation, action["source_event"], identity))
        return bound
