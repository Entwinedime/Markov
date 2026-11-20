"""Capture worker scheduling activity on the monotonic RAW clock."""

import ctypes
import os
import re
from contextlib import contextmanager
from pathlib import Path


class WorkerThread(ctypes.Structure):
    _fields_ = [("local", ctypes.c_int), ("kernel", ctypes.c_uint32)]


class TraceLayout(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint32) for name in ("id", "first", "second")]


class WakeLayout(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint32) for name in ("id", "recipient", "producer", "flags")]


class AscendThreadActivity:
    trace_events = Path("/opt/markov/trace-events")
    host_tasks = Path("/opt/markov/host-proc/self/task")
    kernel_fields = (
        ("sched/sched_switch", (("prev_pid", 4), ("prev_state", 8))),
        ("syscalls/sys_enter_futex", (("uaddr", 8), ("op", 8))),
        ("syscalls/sys_exit_futex", (("ret", 8),)),
    )

    def __init__(self):
        bridge = ctypes.PyDLL(None)
        self.open = bridge.markov_thread_activity_start
        self.open.argtypes = [
            ctypes.POINTER(WorkerThread), ctypes.c_size_t, ctypes.POINTER(TraceLayout),
            ctypes.POINTER(WakeLayout), ctypes.POINTER(ctypes.c_int), ctypes.c_size_t,
        ]
        self.open.restype = ctypes.c_void_p
        self.close = bridge.markov_thread_activity_stop
        self.close.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        self.close.restype = ctypes.c_int

    @classmethod
    def layout(cls, event, layout_type, fields):
        source = cls.trace_events / event
        observed = {name: (int(offset), int(size)) for name, offset, size in re.findall(
            r"field:[^;]*?\b(\w+);\s+offset:(\d+);\s+size:(\d+);", (source / "format").read_text(),
        )}
        if observed.get("common_type") != (0, 2) or any(observed.get(name, (0, 0))[1] != size for name, size in fields):
            raise ValueError(f"unsupported worker tracepoint layout: {event}")
        return layout_type(int((source / "id").read_text()), *(observed[name][0] for name, _ in fields))

    @classmethod
    def owned_threads(cls):
        threads = []
        for task in cls.host_tasks.iterdir():
            try:
                status = (task / "status").read_text()
            except FileNotFoundError:
                continue
            namespace, = (line for line in status.splitlines() if line.startswith("NSpid:"))
            values = namespace.split()
            threads.append(WorkerThread(int(values[-1]), int(values[1])))
        return threads

    @staticmethod
    def online_cpus():
        cpus = []
        for interval in Path("/sys/devices/system/cpu/online").read_text().strip().split(","):
            first, _, last = interval.partition("-")
            cpus.extend(range(int(first), int(last or first) + 1))
        return (ctypes.c_int * len(cpus))(*cpus)

    def start(self, *, detailed=False):
        layouts, wake, cpus = None, None, ()
        if detailed:

            # The host proc view is used only for this worker's kernel thread identities.
            selected = self.owned_threads()
            layouts = (TraceLayout * len(self.kernel_fields))(
                *(self.layout(event, TraceLayout, fields) for event, fields in self.kernel_fields),
            )
            wake = self.layout("sched/sched_waking", WakeLayout, (("pid", 4), ("common_pid", 4), ("common_flags", 1)))
            cpus = self.online_cpus()
        else:
            selected = [WorkerThread(int(task.name), 0) for task in Path("/proc/self/task").iterdir()]

        threads = (WorkerThread * len(selected))(*selected)
        self.handle = self.open(threads, len(threads), layouts, ctypes.byref(wake) if wake is not None else None,
                                cpus if detailed else None, len(cpus))
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
