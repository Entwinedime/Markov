"""Measure the native buffered file path on disposable, verified KV pages."""

from __future__ import annotations

from pathlib import Path
from tempfile import TemporaryDirectory
import time
from typing import TYPE_CHECKING, Any

import torch

from sglang.srt.managers.cache_controller import HiCacheController, PrefetchOperation
from sglang.srt.environ import temp_set_env
from sglang.srt.mem_cache.hicache_storage import HiCacheFile, HiCacheStorageConfig
from sglang.srt.utils.step_timing import _IO_BATCH

if TYPE_CHECKING:
    from markov.adapters.sglang.cache_io import CacheIOSampler


class TimedFile(HiCacheFile):
    """Retain native reads/writes; record their disjoint per-page intervals."""

    def batch_set(
        self,
        keys: list[str],
        values: list[torch.Tensor] | None = None,
        target_locations: Any = None,
        target_sizes: Any = None,
    ) -> bool:
        self.write_calls = []
        self.batch_begin_ns = time.perf_counter_ns()
        result = super().batch_set(keys, values, target_locations, target_sizes)
        self.batch_end_ns = time.perf_counter_ns()
        return result

    def set(
        self, key: str, value: torch.Tensor | None = None, target_location: Any = None, target_sizes: Any = None
    ) -> bool:
        begin = time.perf_counter_ns()
        result = super().set(key, value, target_location, target_sizes)
        end = time.perf_counter_ns()
        self.write_calls.append(end - begin)
        return result

    def get(self, key: str, target_location: torch.Tensor, target_sizes: Any = None) -> torch.Tensor | None:
        begin = time.perf_counter_ns()
        result = super().get(key, target_location, target_sizes)
        end = time.perf_counter_ns()
        self.read_calls.append(end - begin)
        return result


class FileIOSampler:
    def __init__(self, sampler: CacheIOSampler, controller: HiCacheController, directory: Path) -> None:
        self.sampler = sampler
        self.controller = controller
        self.directory = directory
        self.boundaries = []

    def mark(self, stage: str, page: int | None = None, published: bool | None = None) -> None:
        self.boundaries.append((stage, time.perf_counter_ns(), page, published))

    def write(
        self, backend: TimedFile, keys: list[str], indices: torch.Tensor, expected_counts: tuple[int, int]
    ) -> dict[str, int | list[int]]:
        begin = time.perf_counter_ns()
        success = self.controller._generic_page_set(keys, indices)
        end = time.perf_counter_ns()
        if not success or backend.last_write_page_counts != expected_counts:
            raise RuntimeError("Native file write did not take the requested existing/new branches")

        # Batch bookkeeping is outside the nested native set intervals. This
        # includes its loop/return and the small recording overhead, once only.
        return {
            "gather_ns": backend.batch_begin_ns - begin,
            "batch_prepare_ns": end - backend.batch_begin_ns - sum(backend.write_calls),
            "page_ns": list(backend.write_calls),
            "total_ns": end - begin,
        }

    def measure(self, pages: int) -> dict[str, int | list[int]]:
        torch = self.sampler.torch
        controller = self.controller
        host = controller.mem_pool_host
        indices = self.sampler.prepare_indices(controller, pages)
        slots = controller.write(indices)
        controller.ack_write_queue.pop().finish_event.synchronize()
        page_tokens = controller.page_size
        keys = [f"page_{index}" for index in range(pages)]

        with TemporaryDirectory(prefix="file-kv-", dir=self.directory) as scratch:
            config = HiCacheStorageConfig(0, 1, 0, 1, 0, 1, False, False, True, "independent")
            with temp_set_env(
                allow_sglang=True,
                SGLANG_HICACHE_FILE_BACKEND_STORAGE_DIR=scratch,
                SGLANG_HICACHE_FILE_BACKEND_MAX_SIZE="0",
                SGLANG_HICACHE_FILE_BACKEND_MIN_FREE_SPACE="0",
            ):
                backend = TimedFile(config, file_path=scratch)
            controller.storage_backend = backend
            new = self.write(backend, keys, slots, (0, pages))
            existing = self.write(backend, keys, slots, (pages, 0))
            file_bytes = sum(path.stat().st_size for path in Path(backend.file_path).iterdir())

            # Clear destinations outside timing so a successful read must
            # restore the independently written K/V payload, not old values.
            for offset in range(0, len(slots), page_tokens):
                host.set_from_flat_data_page(slots[offset], torch.zeros_like(host.get_dummy_flat_data_page()))
            operation = PrefetchOperation("independent", slots, [])
            operation.hash_value = keys
            backend.read_calls = []
            self.boundaries = []
            token = _IO_BATCH.set(self.mark)
            begin = time.perf_counter_ns()
            try:
                controller._generic_page_get(operation, keys, slots)
            finally:
                end = time.perf_counter_ns()
                _IO_BATCH.reset(token)
            stages = {name: stamp for name, stamp, _, _ in self.boundaries if name in ("allocated", "read")}
            publications = [(stamp, published) for name, stamp, _, published in self.boundaries if name == "publish"]
            if (
                operation.completed_tokens != pages * page_tokens
                or len(publications) != pages
                or not all(ok for _, ok in publications)
            ):
                raise RuntimeError("Native file read did not publish all requested pages")
            for offset in range(0, len(slots), page_tokens):
                data = host.get_data_page(int(slots[offset]), flat=False)
                if not bool(torch.all(data[0] == 1)) or not bool(torch.all(data[1] == 2)):
                    raise RuntimeError("File read changed the expected K/V payload")

            previous = stages["read"]
            copies = []
            for stamp, _ in publications:
                copies.append(stamp - previous)
                previous = stamp
            measurement = {
                "file_allocate_ns": stages["read"] - begin - sum(backend.read_calls),
                "file_read_page_ns": backend.read_calls,
                "file_copy_publish_ns": copies,
                "file_return_ns": end - previous,
                "file_new_gather_ns": new["gather_ns"],
                "file_existing_gather_ns": existing["gather_ns"],
                "file_new_batch_prepare_ns": new["batch_prepare_ns"],
                "file_existing_batch_prepare_ns": existing["batch_prepare_ns"],
                "file_new_page_ns": new["page_ns"],
                "file_existing_page_ns": existing["page_ns"],
                "file_read_total_ns": end - begin,
                "file_new_total_ns": new["total_ns"],
                "file_existing_total_ns": existing["total_ns"],
                "physical_pages_verified": pages,
                "peak_file_bytes": file_bytes,
                "filesystem_device": Path(backend.file_path).stat().st_dev,
            }
            controller.storage_backend = None
        return measurement
