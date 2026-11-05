"""Observe synchronization at torch_npu's native callback boundary."""

from __future__ import annotations

import ctypes
import os
import re
import subprocess
import threading
from bisect import bisect_right
from pathlib import Path
from typing import Any

import torch
import torch_npu
from torch_npu.utils import _npu_trace

from markov_collection.artifacts import write_json
from markov_collection.instrumentation.recording import Recording


class DlInfo(ctypes.Structure):
    _fields_ = [
        ("filename", ctypes.c_char_p),
        ("base", ctypes.c_void_p),
        ("symbol", ctypes.c_char_p),
        ("address", ctypes.c_void_p),
    ]


class SynchronizationObservation:
    def __init__(self, recording: Recording) -> None:
        self.recording = recording
        self.markers = recording.markers
        self.enabled = True
        self.records: list[dict[str, Any]] = []
        self.lock = threading.Lock()
        self.events: dict[int, tuple[int, bool]] = {}
        self.next_event = 0
        self.queue_scope = "stream" if int(os.environ.get("PER_STREAM_QUEUE", "0")) else "device"

        library = ctypes.CDLL("libascendcl.so")
        self.get_device = library.aclrtGetDevice
        self.get_device.argtypes = [ctypes.POINTER(ctypes.c_int32)]
        self.get_device.restype = ctypes.c_int

        self.get_stream = library.aclrtStreamGetId
        self.get_stream.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int32)]
        self.get_stream.restype = ctypes.c_int

        _npu_trace.register_callback_for_npu_stream_synchronization(self.stream, "markov.stream")
        _npu_trace.register_callback_for_npu_device_synchronization(self.device, "markov.device")
        _npu_trace.register_callback_for_npu_event_creation(self.create_event, "markov.event.create")
        _npu_trace.register_callback_for_npu_event_deletion(self.delete_event, "markov.event.delete")
        _npu_trace.register_callback_for_npu_event_record(self.record_event, "markov.event.record")
        _npu_trace.register_callback_for_npu_event_wait(self.wait_event, "markov.event.wait")
        _npu_trace.register_callback_for_npu_event_synchronization(self.synchronize_event, "markov.event.synchronize")

        # Trace activation is process-wide; the native worker owns this observer.
        torch_npu._C._activate_npu_trace(0)

    def set_enabled(self, enabled: bool) -> None:
        native = ctypes.CDLL(None).markov_set_runtime_enabled
        native.argtypes = [ctypes.c_int]
        native.restype = None
        native(int(enabled))
        self.enabled = enabled

    def stream(self, stream: int) -> None:
        self.observe("stream", stream)

    def device(self) -> None:
        self.observe("device", None)

    def create_event(self, event: int) -> None:
        with self.lock:
            self.events[event] = self.next_event, False
            self.next_event += 1

    def delete_event(self, event: int) -> None:
        with self.lock:
            self.events.pop(event, None)

    def record_event(self, event: int, stream: int) -> None:
        self.observe("record_event", stream, event)

    def wait_event(self, event: int, stream: int) -> None:
        self.observe("wait_event", stream, event)

    def synchronize_event(self, event: int) -> None:
        self.observe("synchronize_event", None, event)

    def observe(self, kind: str, stream: int | None, event: int | None = None) -> None:
        if not self.enabled:
            return
        record: dict[str, Any] = {
            "kind": kind,
            "queue_scope": self.queue_scope,
            "operation": self.recording.native_operation(kind),
        }
        capture_callers = event is None

        # Native callbacks may arrive on different threads; reserve the identity before ctypes releases the GIL.
        with self.lock:
            identity = len(self.records)
            self.records.append(record)

            # Events created before observation still receive an identity; a reused handle starts a new lifetime.
            if event is not None:
                if event not in self.events:
                    self.events[event] = self.next_event, False
                    self.next_event += 1
                event_identity, callers_recorded = self.events[event]
                record["event"] = event_identity

                # One trace identifies the event's first observed use; repeated callbacks keep the same lifetime ID.
                capture_callers = not callers_recorded
                self.events[event] = event_identity, True

        with self.markers.scope(f"markov.runtime/{os.getpid()}/{identity}"):
            if capture_callers:
                record["stack"] = torch.utils.get_cpp_backtrace()
            device = ctypes.c_int32(-1)
            native_stream = ctypes.c_int32(-1)
            device_status = self.get_device(ctypes.byref(device))
            stream_status = self.get_stream(stream, ctypes.byref(native_stream)) if stream is not None else -1
            record.update(
                {
                    "device": device.value,
                    "device_status": device_status,
                    "stream": native_stream.value,
                    "stream_status": stream_status,
                    "bytes": 0,
                    "direction": -1,
                    "result": None,
                }
            )

    def write(self, directory: Path) -> None:

        # dladdr supplies the load base; its nearest exported symbol cannot identify a hidden caller.
        locate = ctypes.CDLL(None).dladdr
        locate.argtypes = [ctypes.c_void_p, ctypes.POINTER(DlInfo)]
        locate.restype = ctypes.c_int
        frames = []
        offsets: dict[str, set[str]] = {}
        for record in self.records:
            native = []
            for address, library in re.findall(
                r"frame #\d+: .+ \((0x[0-9a-f]+) in (.+)\)", record.get("stack", "")
            ):
                if Path(library).name != "libtorch_npu.so":

                    # A foreign-library gap must not turn an outer NPU frame into an immediate caller.
                    if native and native[-1][1] is not None:
                        native.append((library, None))
                    continue

                address = int(address, 16)
                image = DlInfo()
                if not locate(address, ctypes.byref(image)):
                    raise RuntimeError(f"native frame has no loaded shared object: {address:#x}")
                offset = hex(address - image.base)
                native.append((library, offset))
                offsets.setdefault(library, set()).add(offset)
            frames.append(native)

        symbols = {
            (library, address): name
            for library, addresses in offsets.items()
            for address, name in self.resolve_callers(library, addresses).items()
        }

        output = []
        for record, native in zip(self.records, frames, strict=True):
            entry = {
                **{key: value for key, value in record.items() if key != "stack"},
                "callers": [symbols[frame] if frame[1] is not None else "<external frame>" for frame in native],
            }

            # Keep source addresses compactly so later symbol interpretation does not require another native run.
            if native:
                entry["frames"] = [{"library": library, "offset": offset} for library, offset in native]
            output.append(entry)

        write_json(directory / f"{os.getpid()}-callbacks.json", output)

    @staticmethod
    def resolve_callers(library: str, addresses: set[str]) -> dict[str, str]:
        table = subprocess.run(
            ["readelf", "--wide", "--symbols", library], check=True, capture_output=True, text=True
        ).stdout
        functions: dict[int, dict[str, int]] = {}
        for line in table.splitlines():
            fields = line.split(maxsplit=7)
            if len(fields) != 8 or fields[3] != "FUNC" or fields[6] == "UND":
                continue
            start = int(fields[1], 16)
            size = int(fields[2], 16 if fields[2].startswith("0x") else 10)
            if size:
                name = fields[7].partition("@")[0]
                functions.setdefault(start, {})[name] = start + size

        starts = sorted(functions)
        matched = {}
        for address in addresses:
            position = int(address, 16)
            preceding = bisect_right(starts, position) - 1
            if preceding < 0:
                continue

            # Stripped libraries expose gaps and aliases; neither proves a unique containing function.
            names = {name for name, end in functions[starts[preceding]].items() if position < end}
            if len(names) == 1:
                matched[address] = names.pop()

        names = sorted(set(matched.values()))
        demangled = subprocess.run(
            ["c++filt"], input="\n".join(names), check=True, capture_output=True, text=True
        ).stdout.splitlines()
        symbols = dict(zip(names, demangled, strict=True))
        return {
            address: symbols[matched[address]] if address in matched else "<unknown function>"
            for address in addresses
        }
