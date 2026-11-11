"""Recover SDK counters for ordering and calibrate measured device time independently."""

from __future__ import annotations

import ctypes
import json
import time
from decimal import Decimal
from pathlib import Path


class AscendClock:
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

        driver = ctypes.CDLL("libascend_hal.so")
        query = driver.halGetDeviceInfo
        query.argtypes = [ctypes.c_uint32, ctypes.c_int32, ctypes.c_int32, ctypes.POINTER(ctypes.c_int64)]
        query.restype = ctypes.c_int
        samples = []
        for _ in range(5):
            native_raw, counter = ctypes.c_int64(), ctypes.c_int64()
            begin = time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)

            # Match the profiler's paired MONOTONIC_RAW / SYS_COUNT query; bracket both driver calls.
            for kind, value in ((14, native_raw), (13, counter)):
                status = query(device.value, 0, kind, ctypes.byref(value))
                if status:
                    raise RuntimeError(f"Ascend clock query {kind} failed: {status}")
            end = time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)
            samples.append({"device": device.value, "counter": counter.value, "begin_raw_ns": begin, "end_raw_ns": end})

        # Minimize acquisition uncertainty, independently of any workload or graph completion time.
        return min(samples, key=lambda sample: sample["end_raw_ns"] - sample["begin_raw_ns"])

    def __init__(self, device: Path, samples: list[dict]) -> None:
        self.first, self.last = samples
        physical = device.name.removeprefix("device_")
        for sample in samples:
            if sample["device"] != int(physical) or sample["begin_raw_ns"] > sample["end_raw_ns"]:
                raise ValueError("device clock calibration has a different device or reversed host interval")
        self.begin_ns = (self.first["begin_raw_ns"] + self.first["end_raw_ns"]) // 2
        self.end_ns = (self.last["begin_raw_ns"] + self.last["end_raw_ns"]) // 2
        self.counts = self.last["counter"] - self.first["counter"]
        if self.counts <= 0 or self.first["end_raw_ns"] >= self.last["begin_raw_ns"]:
            raise ValueError("device clock calibration requires advancing, separated counter samples")

        # Undo the SDK's export formula before applying our independently observed counter/RAW relation.
        start = json.loads((device / f"start_info.{physical}").read_text())
        self.offset_us = Decimal(str(float(start["collectionTimeBegin"]) - float(start["clockMonotonicRaw"]) / 1000))
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
        return self.begin_ns + (counter - self.first["counter"]) * (self.end_ns - self.begin_ns) // self.counts

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

    def interval(self, record):

        # Preserve the export's integer wall/RAW offset; float epoch subtraction loses counter ticks.
        framework = record.get("cat") in {"cpu_op", "enqueue", "dequeue"}
        anchor = self.framework_raw if framework else self.cann_raw
        begin = Decimal(str(record["ts"])) * 1000
        end = begin + Decimal(str(record["dur"])) * 1000
        return tuple(self.anchor + round((value - self.offset - anchor) * self.frequency / 1_000_000_000) for value in (begin, end))
