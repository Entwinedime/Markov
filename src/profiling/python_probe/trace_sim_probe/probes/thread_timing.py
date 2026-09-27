"""Diagnostic-only thread clocks and scheduling counters."""

from __future__ import annotations

import resource
import time
from typing import Any


def _thread_schedstat_snapshot() -> dict[str, int] | None:
    """Read Linux per-thread runtime/run-queue counters when available.

    ``/proc/thread-self/schedstat`` is a constant-size, current-thread file.  It
    does not enumerate process state or capture a Python snapshot.  The second
    field is cumulative time spent runnable on a CPU run queue, which lets a
    later diagnostic distinguish scheduled-out time from blocking/sleeping.
    """

    try:
        # Scope ownership to this read; thread-local integer descriptors would
        # survive worker exit and require a separate cleanup mechanism.
        with open("/proc/thread-self/schedstat", encoding="ascii") as stream:
            fields = stream.read(256).split()
        if len(fields) < 3:
            return None
        values = [int(fields[index]) for index in range(3)]
        if any(value < 0 for value in values):
            return None
        return {
            "schedstat_runtime_ns": values[0],
            "schedstat_runqueue_delay_ns": values[1],
            "schedstat_timeslices": values[2],
        }
    except (OSError, UnicodeError, ValueError):
        return None


def _thread_timing_snapshot(*, boundary: str = "start") -> dict[str, int]:
    """Capture bounded per-thread clocks close to a measured call boundary.

    The CPU clock is sampled last at the start boundary and first at the end
    boundary.  This keeps the bounded ``getrusage``/``schedstat`` diagnostic
    work outside the CPU interval used for the wrapped call.
    """

    if boundary not in {"start", "end"}:
        raise ValueError(f"unsupported thread-timing boundary: {boundary!r}")
    if boundary == "end":
        thread_cpu_ns = time.thread_time_ns()
        usage = resource.getrusage(resource.RUSAGE_THREAD)
        schedstat = _thread_schedstat_snapshot()
    else:
        usage = resource.getrusage(resource.RUSAGE_THREAD)
        schedstat = _thread_schedstat_snapshot()
        thread_cpu_ns = time.thread_time_ns()
    return {
        "thread_cpu_ns": thread_cpu_ns,
        "rusage_user_cpu_us": round(usage.ru_utime * 1_000_000),
        "rusage_system_cpu_us": round(usage.ru_stime * 1_000_000),
        "voluntary_context_switches": int(usage.ru_nvcsw),
        "involuntary_context_switches": int(usage.ru_nivcsw),
        "minor_page_faults": int(usage.ru_minflt),
        "major_page_faults": int(usage.ru_majflt),
        **(schedstat or {}),
    }


def _thread_timing_delta(
    started: dict[str, int] | None,
    ended: dict[str, int] | None,
    wall_start_us: int,
    wall_end_us: int,
) -> dict[str, Any] | None:
    """Project active CPU and wait/scheduling evidence into one event payload."""

    if started is None or ended is None:
        return None
    wall_us = max(0, wall_end_us - wall_start_us)
    raw_thread_cpu_us = max(0, (ended["thread_cpu_ns"] - started["thread_cpu_ns"]) // 1000)
    user_cpu_us = max(0, ended["rusage_user_cpu_us"] - started["rusage_user_cpu_us"])
    system_cpu_us = max(0, ended["rusage_system_cpu_us"] - started["rusage_system_cpu_us"])
    thread_cpu_us = min(wall_us, raw_thread_cpu_us)
    wait_or_scheduled_out_us = max(0, wall_us - thread_cpu_us)
    schedstat_available = all(
        field in started and field in ended
        for field in (
            "schedstat_runtime_ns",
            "schedstat_runqueue_delay_ns",
            "schedstat_timeslices",
        )
    )
    schedstat_fields: dict[str, Any]
    if schedstat_available:
        raw_runqueue_us = max(
            0,
            (ended["schedstat_runqueue_delay_ns"] - started["schedstat_runqueue_delay_ns"]) // 1000,
        )
        runnable_scheduled_out_us = min(wait_or_scheduled_out_us, raw_runqueue_us)
        schedstat_fields = {
            "thread_schedstat_status": "available",
            "thread_schedstat_runtime_us": max(
                0,
                (ended["schedstat_runtime_ns"] - started["schedstat_runtime_ns"]) // 1000,
            ),
            "thread_schedstat_raw_runqueue_delay_us": raw_runqueue_us,
            "thread_runnable_scheduled_out_us": runnable_scheduled_out_us,
            "thread_blocked_or_sleep_us": wait_or_scheduled_out_us - runnable_scheduled_out_us,
            "thread_schedstat_timeslices": max(
                0,
                ended["schedstat_timeslices"] - started["schedstat_timeslices"],
            ),
            "thread_schedstat_runqueue_clipped_to_wall_residual": (raw_runqueue_us > wait_or_scheduled_out_us),
        }
    else:
        schedstat_fields = {
            "thread_schedstat_status": "unavailable",
            "thread_runnable_scheduled_out_us": None,
            "thread_blocked_or_sleep_us": None,
        }
    return {
        "thread_timing_clock": "CLOCK_THREAD_CPUTIME_ID_plus_getrusage_RUSAGE_THREAD",
        "thread_cpu_duration_us": thread_cpu_us,
        "thread_cpu_raw_duration_us": raw_thread_cpu_us,
        "thread_user_cpu_duration_us": user_cpu_us,
        "thread_system_cpu_duration_us": system_cpu_us,
        "thread_rusage_cpu_sampling_skew_us": user_cpu_us + system_cpu_us - raw_thread_cpu_us,
        "thread_rusage_cpu_semantics": (
            "diagnostic_only; getrusage user/system CPU spans bracket the thread-clock interval; "
            "sampling skew is reported, not forced to zero or fitted as service cost"
        ),
        "thread_cpu_clipped_to_wall": raw_thread_cpu_us > wall_us,
        "thread_minor_page_faults": max(0, ended["minor_page_faults"] - started["minor_page_faults"]),
        "thread_major_page_faults": max(0, ended["major_page_faults"] - started["major_page_faults"]),
        "thread_wait_or_scheduled_out_us": wait_or_scheduled_out_us,
        "thread_voluntary_context_switches": max(
            0,
            ended["voluntary_context_switches"] - started["voluntary_context_switches"],
        ),
        "thread_involuntary_context_switches": max(
            0,
            ended["involuntary_context_switches"] - started["involuntary_context_switches"],
        ),
        "thread_timing_semantics": (
            "diagnostic_only; wait_or_scheduled_out includes blocking I/O and off-CPU time "
            "and is never direct service/control fit input"
        ),
        "thread_schedstat_semantics": (
            "diagnostic_only; runnable_scheduled_out is Linux schedstat runqueue delay; "
            "blocked_or_sleep is the conserved wall-minus-CPU-minus-runqueue residual; "
            "neither is a direct fit input without an explicit attribution contract"
        ),
        **schedstat_fields,
    }
