"""Relate an Ascend export to the runtime's recorded Unix clock anchors."""

import json
from pathlib import Path


def trace_clock(trace: Path) -> dict:
    from torch_npu.profiler.analysis._profiler_config import ProfilerConfig
    from torch_npu.profiler.analysis.prof_common_func._path_manager import ProfilerPathManager

    directory = trace.parent.parent
    # The vendor accessor caches a rank; each export needs its own loaded state.
    config = type(ProfilerConfig())()
    config.load_info(str(directory))
    if not config._syscnt_enable:
        raise ValueError("This Ascend capture requires recorded system-counter clock anchors")
    info = json.loads(Path(ProfilerPathManager.get_info_file_path(str(directory))).read_text())
    start, end = info["start_info"], info["end_info"]
    tick = config._start_cnt
    tick_ns = 1000 / config._freq
    exported_ns = config.get_local_time(config.get_timestamp_from_syscnt(tick))
    unix_ns = start["start_monotonic"] + end["collectionTimeEnd"] - end["MonotonicTimeEnd"]
    unix_ns += round((tick - start["start_cnt"]) * tick_ns)
    return {
        "clock": "npu_syscnt",
        "origin_ns": exported_ns,
        "unix_origin_ns": unix_ns,
        "origin_tick": tick,
        "ns_per_tick": tick_ns,
        "unix_reference": {
            "start_tick": start["start_cnt"],
            "start_monotonic_ns": start["start_monotonic"],
            "end_unix_ns": end["collectionTimeEnd"],
            "end_monotonic_ns": end["MonotonicTimeEnd"],
        },
    }
