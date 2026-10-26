"""Observe synchronization at torch_npu's native callback boundary."""

from __future__ import annotations

import ctypes
import os
import re
import subprocess
import threading
from pathlib import Path
from typing import Any

import torch
import torch_npu
from torch_npu.utils import _npu_trace

from markov_collection.artifacts.json_file import write_json


class SynchronizationObservation:
    def __init__(self, markers: Any) -> None:
        self.markers = markers
        self.enabled = True
        self.records: list[dict[str, Any]] = []
        self.lock = threading.Lock()
        self.events: dict[int, int] = {}
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
            self.events[event] = self.next_event
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
        record: dict[str, Any] = {"kind": kind, "queue_scope": self.queue_scope}

        # Native callbacks may arrive on different threads; reserve the identity before ctypes releases the GIL.
        with self.lock:
            identity = len(self.records)
            self.records.append(record)

            # Events created before observation still receive an identity; a reused handle starts a new lifetime.
            if event is not None:
                if event not in self.events:
                    self.events[event] = self.next_event
                    self.next_event += 1
                record["event"] = self.events[event]

        with self.markers.scope(f"markov.runtime/{os.getpid()}/{identity}"):
            if event is None:
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

        # Resolve native callers once after observation; hidden symbols need the matching ELF symbol table.
        frames = []
        offsets: dict[str, set[str]] = {}
        for record in self.records:
            native = []
            for name, offset, library in re.findall(
                r"frame #\d+: (.+) \+ (0x[0-9a-f]+) \([^ ]+ in (.+)\)", record.get("stack", "")
            ):
                if Path(library).name != "libtorch_npu.so":
                    continue

                native.append((name, library, offset))
                if name == "<unknown function>":
                    offsets.setdefault(library, set()).add(offset)
            frames.append(native)

        symbols = {}
        for library, addresses in offsets.items():
            addresses = sorted(addresses)
            lines = subprocess.run(
                ["addr2line", "-f", "-C", "-e", library, *addresses], check=True, capture_output=True, text=True
            ).stdout.splitlines()
            symbols.update(((library, address), name) for address, name in zip(addresses, lines[::2], strict=True))

        output = []
        for record, native in zip(self.records, frames, strict=True):
            callers = [
                symbols[(library, offset)] if name == "<unknown function>" else name for name, library, offset in native
            ]
            output.append({**{key: value for key, value in record.items() if key != "stack"}, "callers": callers})

        write_json(directory / f"{os.getpid()}-callbacks.json", output)
