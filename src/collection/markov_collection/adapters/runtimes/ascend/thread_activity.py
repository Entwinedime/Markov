"""Capture worker scheduling activity on the monotonic RAW clock."""

import ctypes
import os
import re
from contextlib import contextmanager
from pathlib import Path


class TraceLayout(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint32) for name in ("id", "first", "second")]


class AscendThreadActivity:
    trace_events = Path("/opt/markov/trace-events")
    kernel_fields = (
        ("sched/sched_switch", (("prev_pid", 4), ("prev_state", 8))),
        ("sched/sched_waking", (("pid", 4),)),
        ("syscalls/sys_enter_futex", (("uaddr", 8), ("op", 8))),
        ("syscalls/sys_exit_futex", (("ret", 8),)),
    )

    def __init__(self):
        bridge = ctypes.PyDLL(None)
        self.open = bridge.markov_thread_activity_start
        self.open.argtypes = [ctypes.POINTER(ctypes.c_int), ctypes.c_size_t, ctypes.POINTER(TraceLayout)]
        self.open.restype = ctypes.c_void_p
        self.close = bridge.markov_thread_activity_stop
        self.close.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        self.close.restype = ctypes.c_int

    @classmethod
    def kernel_layout(cls):
        layouts = []
        for event, fields in cls.kernel_fields:
            source = cls.trace_events / event
            if not (source / "format").is_file():
                return None
            observed = {name: (int(offset), int(size)) for name, offset, size in re.findall(
                r"field:[^;]*?\b(\w+);\s+offset:(\d+);\s+size:(\d+);", (source / "format").read_text(),
            )}
            if observed.get("common_type") != (0, 2) or any(observed.get(name, (0, 0))[1] != size for name, size in fields):
                raise ValueError(f"unsupported worker tracepoint layout: {event}")
            layout = TraceLayout(int((source / "id").read_text()), *(observed[name][0] for name, _ in fields))
            layouts.append(layout)
        return (TraceLayout * len(layouts))(*layouts)

    def start(self, *, detailed=False):
        tids = [int(thread.name) for thread in Path("/proc/self/task").iterdir()]
        threads = (ctypes.c_int * len(tids))(*tids)
        layouts = self.kernel_layout() if detailed else None
        self.handle = self.open(threads, len(tids), layouts)
        if not self.handle:
            raise RuntimeError("cannot open CPU activity observation for the native worker")

    def finish(self, output):
        if self.close(self.handle, os.fsencode(output)):
            raise RuntimeError("cannot publish native CPU activity evidence")

    @contextmanager
    def record(self, output):
        self.start(detailed=True)
        try:
            yield
        finally:
            self.finish(output)
