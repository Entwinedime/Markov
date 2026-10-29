"""Relate an Ascend export to the runtime's recorded Unix clock anchors."""

import json
from pathlib import Path
import time


def sample_clock():
    import torch.distributed as dist
    import torch_npu

    profiler = torch_npu._C._profiler
    frequency = profiler._get_freq()
    if not frequency:
        return None

    samples = []
    for _ in range(5):
        before = profiler._get_syscnt()
        unix_ns = time.time_ns()
        after = profiler._get_syscnt()
        samples.append({"before": before, "unix_ns": unix_ns, "after": after})
    return {"rank": dist.get_rank(), "samples": samples, "ns_per_tick": 1000 / frequency}


def trace_clock(trace: Path, anchors: dict) -> dict:
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
    anchor = anchors[info["rank_id"]]
    samples = anchor["samples"]
    tick = samples[0]["before"]
    tick_ns = 1000 / config._freq
    if anchor["ns_per_tick"] != tick_ns:
        raise ValueError("The observed counter and exporter disagree on clock frequency")

    # Separate exporter clock reads can straddle scheduling; use the worker's
    # counter brackets without inferring an offset from operation durations.
    low = max(sample["unix_ns"] - round((sample["after"] - tick) * tick_ns) for sample in samples)
    high = min(sample["unix_ns"] - round((sample["before"] - tick) * tick_ns) for sample in samples)
    if low > high:
        raise ValueError("The observed counter/Unix brackets have no common clock offset")
    exported_ns = config.get_local_time(config.get_timestamp_from_syscnt(tick))
    return {
        "clock": "npu_syscnt",
        "origin_ns": exported_ns,
        "unix_origin_ns": (low + high) // 2,
        "origin_tick": tick,
        "ns_per_tick": tick_ns,
        "unix_origin_bounds_ns": [low, high],
        "counter_brackets": samples,
        "unix_reference": {
            "start_tick": start["start_cnt"],
            "start_monotonic_ns": start["start_monotonic"],
            "end_unix_ns": end["collectionTimeEnd"],
            "end_monotonic_ns": end["MonotonicTimeEnd"],
        },
    }
