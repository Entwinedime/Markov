"""Capture-tail policy for asynchronous profiling evidence."""

from __future__ import annotations

from typing import Any


def post_workload_drain_seconds(cfg: dict[str, Any]) -> float:
    """Return the capture-only drain after the formal workload ends.

    The workload report remains the source of the formal E2E interval. Keeping
    trace channels alive briefly only retains asynchronous completion/lifecycle
    evidence that causally belongs to the final request.
    """

    value = float(cfg.get("post_workload_drain_sec", 0))
    if value < 0:
        raise ValueError("post_workload_drain_sec must be non-negative")
    return value


def python_probe_flush_interval_seconds(cfg: dict[str, Any]) -> float:
    """Return the optional low-frequency probe flush interval.

    Event-count flushing remains the hot-path policy. This interval only
    bounds how long the final partial batch may remain in userspace while the
    post-workload drain keeps the profiled process alive.
    """

    profiling = cfg.get("profiling") if isinstance(cfg.get("profiling"), dict) else {}
    python_probe = profiling.get("python_probe") if isinstance(profiling.get("python_probe"), dict) else {}
    raw = python_probe.get("flush_interval_sec", 0)
    try:
        value = float(raw)
    except (TypeError, ValueError) as error:
        raise ValueError("profiling.python_probe.flush_interval_sec must be numeric") from error
    if value < 0:
        raise ValueError("profiling.python_probe.flush_interval_sec must be non-negative")
    return value


def capture_tail_policy(cfg: dict[str, Any]) -> dict[str, float]:
    """Validate and return the capture-tail timing relationship."""

    drain_sec = post_workload_drain_seconds(cfg)
    flush_interval_sec = python_probe_flush_interval_seconds(cfg)
    if flush_interval_sec > 0 and drain_sec < flush_interval_sec:
        raise ValueError(
            "post_workload_drain_sec must be at least profiling.python_probe.flush_interval_sec "
            "when periodic probe flushing is enabled"
        )
    return {
        "post_workload_drain_sec": drain_sec,
        "python_probe_flush_interval_sec": flush_interval_sec,
    }
