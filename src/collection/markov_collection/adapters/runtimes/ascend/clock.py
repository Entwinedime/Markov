"""Recover SDK host coordinates and RAW time, and calibrate measured device time."""

from __future__ import annotations

import ctypes
import json
from bisect import bisect_right
from decimal import Decimal
from pathlib import Path


class DeviceCalibration:
    """Map device counter ticks between independently bracketed RAW snapshots."""

    def __init__(self, samples: list[dict]) -> None:
        if len(samples) < 2:
            raise ValueError("device clock requires at least two independent samples")
        self.device_id = samples[0]["device"]
        if (any(sample["device"] != self.device_id or sample["begin_raw_ns"] > sample["end_raw_ns"] for sample in samples)
                or any(left["counter"] >= right["counter"] or left["end_raw_ns"] >= right["begin_raw_ns"]
                       for left, right in zip(samples, samples[1:]))):
            raise ValueError("device clock requires advancing, separated calibration intervals from one device")
        self.samples = samples
        self.counters = [sample["counter"] for sample in samples]
        self.midpoints = [(sample["begin_raw_ns"] + sample["end_raw_ns"]) // 2 for sample in samples]

    def counter_segment(self, counter: int) -> int:
        return min(max(bisect_right(self.counters, counter) - 1, 0), len(self.counters) - 2)

    def raw_timestamp(self, counter: int) -> int:
        index = self.counter_segment(counter)
        begin, end = self.counters[index:index + 2]
        start, finish = self.midpoints[index:index + 2]
        return start + (counter - begin) * (finish - start) // (end - begin)

    def counter_at(self, timestamp: int) -> tuple[int, int]:
        index = min(max(bisect_right(self.midpoints, timestamp) - 1, 0), len(self.midpoints) - 2)
        start, finish = self.midpoints[index:index + 2]
        begin, end = self.counters[index:index + 2]
        counts, duration = end - begin, finish - start

        # Invert the integer floor exactly so a recorded endpoint identifies one physical tick.
        first = -((start - timestamp) * counts // duration)
        last = -((start - timestamp - 1) * counts // duration) - 1
        if first != last or not 0 <= first <= counts:
            raise ValueError("device timestamp does not identify one counter inside its sampled clock segment")
        return index, begin + first

    def bounds(self, timestamp: int) -> tuple[int, int]:
        index, counter = self.counter_at(timestamp)
        first, last = self.samples[index:index + 2]
        offset, counts = counter - first["counter"], last["counter"] - first["counter"]
        start, finish = first["begin_raw_ns"], last["begin_raw_ns"]
        lower = start + offset * (finish - start) // counts
        start, finish = first["end_raw_ns"], last["end_raw_ns"]
        upper = start - (offset * (start - finish) // counts)
        return lower, upper


class AscendClock(DeviceCalibration):
    @staticmethod
    def sample(user_device: int) -> dict:
        acl = ctypes.CDLL("libascendcl.so")
        device_id = acl.aclrtGetLogicDevIdByUserDevId
        device_id.argtypes = [ctypes.c_int32, ctypes.POINTER(ctypes.c_int32)]
        device_id.restype = ctypes.c_int
        device = ctypes.c_int32()
        status = device_id(user_device, ctypes.byref(device))
        if status:
            raise RuntimeError(f"Ascend clock device mapping failed: {status}")

        sample = ctypes.CDLL(None).markov_sample_device
        sample.argtypes = [ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint64)]
        sample.restype = ctypes.c_int
        values = (ctypes.c_uint64 * 3)()

        # The native helper brackets only the actual queries; Python releases the GIL outside that interval.
        status = sample(device.value, values)
        if status:
            raise RuntimeError(f"Ascend native clock acquisition failed: {status}")
        counter, begin, end = values
        return {"device": device.value, "counter": counter, "begin_raw_ns": begin, "end_raw_ns": end}

    def __init__(self, device: Path, samples: list[dict]) -> None:
        super().__init__(samples)
        physical = device.name.removeprefix("device_")
        if self.device_id != int(physical):
            raise ValueError("device clock calibration has a different device from the SDK profile")
        self.begin_ns, self.end_ns = self.midpoints[0], self.midpoints[-1]

        # Keep the epoch offset exact; float-to-string conversion can lose several device counter ticks.
        start = json.loads((device / f"start_info.{physical}").read_text())
        self.offset_us = Decimal(str(start["collectionTimeBegin"])) - Decimal(start["clockMonotonicRaw"]) / 1000
        info = json.loads((device / f"info.json.{physical}").read_text())
        self.frequency = Decimal(info["DeviceInfo"][0]["hwts_frequency"]) * 1_000_000
        host = self.values(device / f"host_start.log.{physical}")
        native = self.values(device / f"dev_start.log.{physical}")
        host_frequency = Decimal(info["CPU"][0]["Frequency"]) * 1_000_000
        self.host_anchor = host["clock_monotonic_raw"] + round(Decimal(host["cntvct_diff"]) * 1_000_000_000 / host_frequency)
        self.counter_anchor = native["cntvct"]

    @staticmethod
    def values(path: Path) -> dict[str, int]:
        return {key.strip(): int(value.strip()) for line in path.read_text().splitlines()
                if ":" in line for key, value in [line.split(":", 1)]}

    def timestamp(self, microseconds: Decimal) -> int:
        raw = (microseconds - self.offset_us) * 1000
        counter = self.counter_anchor + round((raw - self.host_anchor) * self.frequency / 1_000_000_000)

        # Extrapolated SDK setup/teardown records are filtered outside the valid observation window.
        return self.raw_timestamp(counter)

    def interval(self, record: dict) -> tuple[int, int]:
        begin = Decimal(str(record["ts"]))
        return self.timestamp(begin), self.timestamp(begin + Decimal(str(record["dur"])))


class HostCounter:
    def __init__(self, profile: Path) -> None:
        start, = profile.glob("PROF_*/host/start_info")
        host, = profile.glob("PROF_*/host/host_start.log")
        device, = profile.glob("PROF_*/device_*")
        info = device / f"info.json.{device.name.removeprefix('device_')}"
        start = json.loads(start.read_text())
        anchor = AscendClock.values(host)
        info = json.loads(info.read_text())
        self.frequency = Decimal(info["CPU"][0]["Frequency"]) * 1_000_000
        self.anchor = anchor["cntvct"]
        self.framework_raw = anchor["clock_monotonic_raw"]
        self.cann_raw = self.framework_raw + round(Decimal(anchor["cntvct_diff"]) * 1_000_000_000 / self.frequency)
        self.offset = Decimal(start["collectionTimeBegin"]) * 1000 - int(start["clockMonotonicRaw"])

    def timestamp(self, counter: int) -> int:

        # The profiler records this counter/RAW anchor; recover time on its actual counter grid.
        return self.framework_raw + round(Decimal(counter - self.anchor) * 1_000_000_000 / self.frequency)

    def interval(self, record):

        # Preserve the export's integer wall/RAW offset; float epoch subtraction loses counter ticks.
        framework = record.get("cat") in {"cpu_op", "enqueue", "dequeue"}
        anchor = self.framework_raw if framework else self.cann_raw
        begin = Decimal(str(record["ts"])) * 1000
        end = begin + Decimal(str(record["dur"])) * 1000
        return tuple(self.anchor + round((value - self.offset - anchor) * self.frequency / 1_000_000_000) for value in (begin, end))
