"""Export the actual Torch NPU clock transform, not an estimated trace offset."""

from __future__ import annotations

from pathlib import Path


def read_host_clock(trace: Path) -> dict | None:
    root = trace.parent.parent
    # Other profiler backends do not have this counter domain.
    if not list(root.glob("profiler_info*.json")) or not root.name.endswith("_ascend_pt"):
        return None
    from torch_npu.profiler.analysis._profiler_config import ProfilerConfig

    # The vendor accessor is a singleton that caches the first rank. Use a
    # fresh underlying instance for each file, without changing that singleton.
    config = type(ProfilerConfig())()
    config.load_info(str(root))
    if config._syscnt_enable:
        origin = config._start_cnt
        scale = 1000.0 / config._freq
    else:
        origin, scale = 0, 1.0
    return {"clock": "npu_syscnt", "origin_tick": origin,
            "origin_ns": config.get_local_time(config.get_timestamp_from_syscnt(origin)), "ns_per_tick": scale}
