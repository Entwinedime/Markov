"""Trace-channel normalization shared by runner and manifest inspection."""

from __future__ import annotations

from collections.abc import Iterable
from typing import Any


TRACE_CHANNELS = frozenset({"torch", "ld_preload", "python_probe"})


def configured_trace_channels(config: dict[str, Any]) -> tuple[str, ...] | None:
    """Return selected channels, using ``None`` to represent all channels."""

    cpp_config = config.get("cpp_trace_graph") if isinstance(config.get("cpp_trace_graph"), dict) else {}
    raw = cpp_config.get("trace_channels")
    if raw is None:
        return None
    if isinstance(raw, str):
        return normalize_trace_channels(raw.split(","))
    if isinstance(raw, list):
        return normalize_trace_channels(raw)
    raise TypeError("trace_channels must be a comma-separated string or a list")


def normalize_trace_channels(values: Iterable[Any]) -> tuple[str, ...] | None:
    """Normalize channels in input order, with ``all`` selecting every channel."""

    channels: list[str] = []
    for value in values:
        token = str(value).strip().lower()
        if not token:
            continue
        if token == "all":
            return None
        if token not in TRACE_CHANNELS:
            raise ValueError(f"unknown trace channel: {token}")
        if token not in channels:
            channels.append(token)
    return tuple(channels) or None
