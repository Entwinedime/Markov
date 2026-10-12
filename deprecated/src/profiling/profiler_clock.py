"""Export the actual Torch NPU clock transform, not an estimated trace offset."""

from __future__ import annotations

import json
from pathlib import Path


def read_host_clock(trace: Path) -> dict | None:
    root = trace.parent.parent
    # Other profiler backends do not have this counter domain.
    if not list(root.glob("profiler_info*.json")) or not root.name.endswith("_ascend_pt"):
        return None
    from torch_npu.profiler.analysis._profiler_config import ProfilerConfig
    from torch_npu.profiler.analysis.prof_common_func._path_manager import ProfilerPathManager

    # The vendor accessor is a singleton that caches the first rank. Use a
    # fresh underlying instance for each file, without changing that singleton.
    config = type(ProfilerConfig())()
    config.load_info(str(root))
    if config._syscnt_enable:
        origin = config._start_cnt
        scale = 1000.0 / config._freq
    else:
        origin, scale = 0, 1.0
    clock = {
        "clock": "npu_syscnt",
        "origin_tick": origin,
        "origin_ns": config.get_local_time(config.get_timestamp_from_syscnt(origin)),
        "ns_per_tick": scale,
    }
    info = json.loads(Path(ProfilerPathManager.get_info_file_path(str(root))).read_text(encoding="utf-8"))
    if config._syscnt_enable:
        start, end = info["start_info"], info["end_info"]
        reference = {
            "start_tick": start["start_cnt"],
            "start_monotonic_ns": start["start_monotonic"],
            "end_unix_ns": end["collectionTimeEnd"],
            "end_monotonic_ns": end["MonotonicTimeEnd"],
        }
        # Torch's export prefers CANN's independently sampled anchors. The
        # probe and LD hook use CLOCK_REALTIME, so retain the framework's own
        # counter/monotonic and monotonic/realtime pairs as the common epoch.
        # This is a recorded clock transform, not a fit to operation timing.
        clock["unix_origin_ns"] = (
            reference["start_monotonic_ns"]
            + reference["end_unix_ns"]
            - reference["end_monotonic_ns"]
            + int((origin - reference["start_tick"]) * scale)
        )
        clock["unix_reference"] = reference

    return clock
