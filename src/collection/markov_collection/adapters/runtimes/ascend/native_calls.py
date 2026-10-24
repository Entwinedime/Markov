"""Join native API arguments through their own thread, entry point and clock mapping."""

from __future__ import annotations

import json
from collections import defaultdict
from decimal import Decimal
from itertools import pairwise
from pathlib import Path
from typing import Any


def event_time_ns(event: dict[str, Any], field: str = "ts") -> int:
    return int(Decimal(str(event[field])) * 1000)


class NativeRuntimeCalls:
    def __init__(self, directory: Path, profiler: Path) -> None:
        if not directory.is_dir():
            raise NotADirectoryError(directory)

        clocks = list(profiler.glob("PROF*/host/start_info"))
        if len(clocks) != 1:
            raise ValueError("native runtime arguments need one CANN host clock mapping")

        clock = json.loads(clocks[0].read_text())
        self.offset_ns = int(Decimal(str(clock["collectionTimeBegin"])) * 1000) - int(clock["clockMonotonicRaw"])

        self.calls: dict[tuple[int, str], list[dict[str, Any]]] = defaultdict(list)
        for path in directory.glob("*.jsonl"):
            for index, line in enumerate(path.read_text().splitlines()):
                call = json.loads(line)
                if call["end_raw_ns"] < call["begin_raw_ns"]:
                    raise ValueError("native runtime call ends before it begins")
                call["source"] = {"process": int(path.stem), "record": index}
                self.calls[(call["thread"], call["api"])].append(call)

        self.callbacks: dict[str, dict[str, Any]] = {}
        for path in directory.glob("*-callbacks.json"):
            process = int(path.stem.removesuffix("-callbacks"))
            for identity, call in enumerate(json.loads(path.read_text())):
                call["source"] = {"process": process, "callback": identity}
                call.update(self.synchronization_protocol(call))
                self.callbacks[f"markov.runtime/{process}/{identity}"] = call

        self.callback_apis = {call["api"] for call in self.callbacks.values() if call["api"] is not None}

    @staticmethod
    def synchronization_protocol(call: dict[str, Any]) -> dict[str, Any]:
        event_apis = {"record_event": "aclrtRecordEvent", "wait_event": "aclrtStreamWaitEvent"}
        if call["kind"] in event_apis:
            return {"api": event_apis[call["kind"]], "position": "after", "queue_scope": None}
        if call["kind"] == "device":
            drained = any(
                name.startswith(("c10_npu::npuSynchronizeDevice(", "c10_npu::npuSynchronizeUsedDevices("))
                for name in call["callers"]
            )
            return {
                "api": "aclrtSynchronizeDeviceWithTimeout",
                "position": "after",
                "queue_scope": "device" if drained else None,
            }

        entry_points = (
            ("c10_npu::acl::AclrtSynchronizeStreamWithTimeout(", "aclrtSynchronizeStreamWithTimeout", "before"),
            ("c10_npu::stream_synchronize(", "aclrtSynchronizeStream", "before"),
            ("at_npu::native::copy_between_host_and_device_opapi(", "aclrtSynchronizeStream", "after"),
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
            for prefix, api, position in entry_points:
                if name.startswith(prefix):
                    caller = name if position == "after" else (callers[index + 1] if index + 1 < len(callers) else "")
                    return {
                        "api": api,
                        "position": position,
                        "queue_scope": call["queue_scope"] if caller.startswith(accessors) else None,
                    }

        return {"api": None, "position": None, "queue_scope": None}

    def arguments(self, event: dict[str, Any], timestamp_ns: int) -> dict[str, Any] | None:
        arguments = event["args"]
        instant = timestamp_ns - self.offset_ns
        matches = [
            call
            for call in self.calls.get((arguments["Thread Id"], arguments["id"]), ())
            if call["begin_raw_ns"] <= instant < call["end_raw_ns"]
        ]
        if len(matches) != 1:
            return None

        call = matches[0]

        # The API entry identifies the invocation; profiler closing timestamps are not service costs.
        return self.parameters(call)

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
    def scope_key(thread: int, api: str, owner: dict[str, Any]) -> tuple[Any, ...]:
        return (thread, api, *(owner[name] for name in ("operation", "region", "baseline", "observer", "auxiliary")))

    @staticmethod
    def callback_origin(action: dict[str, Any]) -> int:

        # Event callbacks follow LaunchRecord/WaitEventTask on its producer, even when the API runs on a worker.
        queue = action["submission"]["queue"]
        if action["kind"] in ("record_event", "wait_event") and queue is not None:
            return queue["enqueue_event"]
        return action["source_event"]

    def bind_callbacks(
        self, events: list[dict[str, Any]], actions: list[dict[str, Any]], owners: dict[int, dict[str, Any]]
    ) -> None:
        markers: dict[tuple[Any, ...], list[int]] = defaultdict(list)
        calls: dict[tuple[Any, ...], list[dict[str, Any]]] = defaultdict(list)
        for identity, event in enumerate(events):
            callback = self.callbacks.get(event.get("name"))
            if callback is not None:
                key = self.scope_key(event["tid"], callback["api"], owners[identity])
                markers[key].append(identity)

        for action in actions:
            if action["native_api"] in self.callback_apis:
                identity = self.callback_origin(action)
                event = events[identity]
                thread = event.get("args", {}).get("Thread Id", event["tid"])
                key = self.scope_key(thread, action["native_api"], owners[action["source_event"]])
                calls[key].append(action)

        for key, identities in markers.items():
            actions_in_scope = sorted(
                calls[key], key=lambda action: event_time_ns(events[self.callback_origin(action)])
            )
            identities.sort(key=lambda identity: event_time_ns(events[identity]))
            if len(identities) != len(actions_in_scope):
                continue

            ordered = []
            for identity, action in zip(identities, actions_in_scope, strict=True):
                pair = [events[identity], events[self.callback_origin(action)]]
                if self.callbacks[events[identity]["name"]]["position"] == "after":
                    pair.reverse()
                ordered.extend(pair)

            # The native callback protocol alternates one marker and one API in the same owning scope.
            if any(
                event_time_ns(left) + event_time_ns(left, "dur") > event_time_ns(right)
                for left, right in pairwise(ordered)
            ):
                continue

            for identity, action in zip(identities, actions_in_scope, strict=True):
                callback = self.callbacks[events[identity]["name"]]
                parameters = self.parameters(callback)
                direct = action["native_call"]
                if direct is not None:
                    for resource in ("device", "stream"):
                        status = f"{resource}_status"
                        if direct[status] == parameters[status] == 0 and direct[resource] != parameters[resource]:
                            raise ValueError("native synchronization callback disagrees with the API arguments")
                    parameters = {**parameters, **direct, "source": {**parameters["source"], **direct["source"]}}

                parameters["queue_scope"] = callback.get("queue_scope")
                action["native_call"] = parameters
