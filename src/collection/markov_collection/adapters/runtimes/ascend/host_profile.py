"""Record native queue cuts and per-thread CPU activity around a serving window."""

import ctypes
import os
from pathlib import Path
from time import CLOCK_MONOTONIC_RAW, clock_gettime_ns

from markov_adapters.artifacts import write_json
from markov_collection.adapters.runtimes.ascend.thread_activity import AscendThreadActivity


class AscendSpecializations:
    def __init__(self) -> None:
        from triton.runtime.jit import JITFunction

        self.runtime = JITFunction
        self.started = False
        self.loads = []

    def start(self) -> None:
        self.previous_hook = self.runtime.cache_hook
        self.runtime.cache_hook = self.loaded
        self.started = True

    def loaded(self, **event):
        function = event["fn"]
        self.loads.append({
            "kernel": f"{function.module}.{function.name}",
            "specialization": str(event["key"]),
            "raw_ns": clock_gettime_ns(CLOCK_MONOTONIC_RAW),
        })
        return self.previous_hook(**event) if self.previous_hook is not None else False

    def finish(self) -> list[dict]:
        self.runtime.cache_hook = self.previous_hook
        return self.loads


class AscendHostProfile:
    def __init__(self, directory: Path) -> None:
        self.directory = directory
        self.started = False
        self.specializations = AscendSpecializations()
        self.activity = AscendThreadActivity()
        bridge = ctypes.PyDLL(None)
        self.start_runtime = bridge.markov_runtime_timing_start
        self.start_runtime.argtypes = []
        self.start_runtime.restype = None
        self.stop_runtime = bridge.markov_runtime_timing_stop
        self.stop_runtime.argtypes = [ctypes.c_char_p]
        self.stop_runtime.restype = ctypes.c_int
        self.host_clock = bridge.markov_sample_host_clock
        self.host_clock.argtypes = [ctypes.POINTER(ctypes.c_uint64)]
        self.host_clock.restype = None

    def clock_sample(self):
        values = (ctypes.c_uint64 * 3)()
        self.host_clock(values)
        return list(values)

    def start(self) -> None:
        from torch.autograd import _enable_record_function
        from torch_npu import profiler

        self.profiler = profiler.profile(
            activities=[profiler.ProfilerActivity.CPU],
            on_trace_ready=profiler.tensorboard_trace_handler(str(self.directory / "host-profile"), analyse_flag=False),
        )
        self.begin_clock = self.clock_sample()
        self.profiler.start()

        # The SDK still emits queue marks with ATen range callbacks disabled.
        _enable_record_function(False)
        self.activity.start()
        self.specializations.start()
        self.start_runtime()
        self.started = True

    def finish(self) -> dict:
        from torch.autograd import _enable_record_function

        specializations = self.specializations.finish()
        runtime = self.directory / "runtime-timing.jsonl"
        activity = self.directory / "thread-activity.jsonl"
        runtime_status = self.stop_runtime(os.fsencode(runtime))
        try:
            self.activity.finish(activity)
        finally:
            try:
                self.profiler.stop()
            finally:
                _enable_record_function(True)
        if runtime_status:
            raise RuntimeError("cannot publish native CPU timing evidence")
        profile, = (self.directory / "host-profile").glob("*_ascend_pt")
        queues = self.directory / "host-queues.json"
        write_json(queues, self.queue_intervals(profile, (self.begin_clock, self.clock_sample())), replace=False)
        return {
            "profile": str(profile.relative_to(self.directory)), "queues": queues.name,
            "activity": activity.name, "runtime": runtime.name,
            "specialization_loads": specializations,
        }

    @staticmethod
    def queue_intervals(profile: Path, clock_samples) -> dict:
        from torch_npu.profiler.analysis._profiler_config import ProfilerConfig
        from torch_npu.profiler.analysis.prof_parse._fwk_file_parser import FwkFileParser

        configuration = ProfilerConfig()
        configuration.load_info(str(profile))
        enqueues, dequeues = FwkFileParser(str(profile)).get_task_queue_data()

        # Queue records use the SDK startup counter pair. Bound its common offset with independent RAW readings.
        corrections = []
        for counter, begin, end in clock_samples:
            sdk_ns = configuration.get_timestamp_from_syscnt(counter)
            corrections.append((begin - sdk_ns, end - sdk_ns))
        lower = max(begin for begin, _ in corrections)
        upper = min(end for _, end in corrections)
        if lower > upper:
            raise ValueError("CPU queue timestamps have no common host clock calibration")
        offset_ns = configuration.get_local_time(0) - (lower + upper) // 2

        def interval(record):
            begin_ns = record.ts - offset_ns
            return [record.tid, begin_ns, begin_ns + record.dur]

        produced = {record.corr_id: record for record in enqueues}
        consumed = {record.corr_id: record for record in dequeues}
        if len(produced) != len(enqueues) or len(consumed) != len(dequeues):
            raise ValueError("host queue evidence repeats a native correlation")
        return {
            "clock": "monotonic_raw_ns",
            "clock_samples": clock_samples,
            "raw_correction_ns": [lower, upper],
            "unpaired": len(produced.keys() ^ consumed.keys()),
            "commands": [
                [command.origin_name, interval(produced[key]), interval(command)]
                for key, command in consumed.items() if key in produced
            ],
        }
