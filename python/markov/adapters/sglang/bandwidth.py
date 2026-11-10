"""Pace real HiCache transfers with shared, one-operation-burst service quotas."""

from __future__ import annotations

import fcntl
import json
import logging
import math
import mmap
import os
from pathlib import Path
import struct
import threading
import time

from markov.artifacts.writing import write_json


DIRECTIONS = ("device_to_host", "host_to_device", "host_to_storage", "storage_to_host")


def limits(raw: dict) -> dict[str, float]:
    fields = {*DIRECTIONS, "storage_shared"}
    if not isinstance(raw, dict) or set(raw) != fields:
        raise ValueError("Declare all five HiCache bandwidth limits in bytes/sec")
    if any(type(value) not in (float, int) or not math.isfinite(value) or value <= 0 for value in raw.values()):
        raise ValueError("HiCache transfer bandwidth must be finite positive bytes/sec")
    return {key: float(raw[key]) for key in fields}


class BandwidthPacer:
    def __init__(self, rates: dict, participants: int, rank: int, path: Path):
        self.rates = limits(rates)
        self.participants, self.rank = participants, rank
        self.lock = threading.Lock()
        self.file = path.open("a+b", buffering=0)
        size = (2 * participants + 3) * 8
        with self.lock:
            fcntl.flock(self.file, fcntl.LOCK_EX)
            try:
                if os.fstat(self.file.fileno()).st_size < size:
                    self.file.truncate(size)
                self.state = mmap.mmap(self.file.fileno(), size)
            finally:
                fcntl.flock(self.file, fcntl.LOCK_UN)

    def admit(self, direction: int, size: int) -> None:
        lane = 2 * self.rank + direction if direction < 2 else 2 * self.participants + direction - 2
        channels = [(lane, self.rates[DIRECTIONS[direction]])]
        if direction >= 2:
            channels.append((2 * self.participants + 2, self.rates["storage_shared"]))

        # The first transfer can start now; later starts respect every reservation.
        with self.lock:
            fcntl.flock(self.file, fcntl.LOCK_EX)
            try:
                start = max(
                    time.monotonic_ns(),
                    *(struct.unpack_from("<Q", self.state, channel * 8)[0] for channel, _ in channels),
                )
                for channel, rate in channels:
                    struct.pack_into("<Q", self.state, channel * 8, start + math.ceil(size * 1e9 / rate))
            finally:
                fcntl.flock(self.file, fcntl.LOCK_UN)
        remaining = start - time.monotonic_ns()
        if remaining > 0:
            time.sleep(remaining / 1e9)


class TransferQuotas:
    def __init__(self, rates: dict):
        self.rates = limits(rates)
        self.pacer = None

    def controller(self, original, controller, *args, **kwargs):
        from sglang.srt.distributed.parallel_state import (
            get_tensor_model_parallel_rank,
            get_tensor_model_parallel_world_size,
        )

        result = original(controller, *args, **kwargs)
        rank, participants = get_tensor_model_parallel_rank(), get_tensor_model_parallel_world_size()
        path = Path("/capture/transfer_admission.bin")
        self.pacer = BandwidthPacer(self.rates, participants, rank, path)
        write_json(
            path.with_name(f"transfer_bandwidth_rank_{rank}.json"),
            {
                "kind": "paced_transfer_admission",
                "rank": rank,
                "participants": participants,
                "bytes_per_sec": self.rates,
                "burst": "one native operation",
                "scope": "PCIe directions per rank; file directions and shared media across all ranks",
            },
        )
        return result

    def backup(self, original, controller):
        size = sum(operation.device_indices.numel() for operation in controller.write_queue)
        if size:
            self.pacer.admit(0, size * controller.mem_pool_host.get_size_per_token())
        return original(controller)

    def load(self, original, controller):
        size = sum(operation.host_indices.numel() for operation in controller.load_queue)
        if size:
            self.pacer.admit(1, size * controller.mem_pool_host.get_size_per_token())
        return original(controller)

    def reserve(self, original, evictor, suffixed, size, **kwargs):
        accepted = original(evictor, suffixed, size, **kwargs)
        if accepted:
            self.pacer.admit(2, size)
        return accepted

    def read(self, _original, backend, key, target_location, target_sizes=None):
        import torch

        path = backend._get_component_path(key)
        try:
            expected = target_location.numel() * target_location.element_size()
            # Opening first retains the native in-flight read if LRU unlinks its key.
            with open(path, "rb", buffering=0) as stream:
                self.pacer.admit(3, expected)
                buffer = memoryview(target_location.view(torch.uint8).contiguous().numpy())
                if stream.readinto(buffer) != expected:
                    raise OSError(f"Short HiCache read: {key}")
            backend._evictor.touch(backend._get_suffixed_key(key), path)
            return target_location
        except FileNotFoundError:
            logging.getLogger(__name__).warning("Failed to fetch %s from HiCacheFile storage", key)
            return None

    def install(self) -> None:
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        controller = "sglang.srt.managers.cache_controller.HiCacheController."
        for method, wrapper in (
            ("__init__", self.controller),
            ("start_writing", self.backup),
            ("start_loading", self.load),
        ):
            HookRegistry.register(controller + method, wrapper, HookType.AROUND)
        HookRegistry.register("sglang.srt.mem_cache.hicache_storage.HiCacheFile.get", self.read, HookType.AROUND)
        HookRegistry.register(
            "sglang.srt.mem_cache.storage.file.lru_file_evictor.LRUFileEvictor.reserve", self.reserve, HookType.AROUND
        )


def install() -> None:
    if raw := os.environ.get("MARKOV_TRANSFER_BANDWIDTH"):
        TransferQuotas(json.loads(raw)).install()
