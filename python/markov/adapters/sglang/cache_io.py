"""Measure real HiCache controller responses without model weights or a profiler."""

import argparse
import gc
import json
from pathlib import Path
import threading
import time

from markov.artifacts.request import positive_integer, read_json, reject_unknown


class CacheIOSampler:
    def __init__(self, device: int, geometry: dict):
        import torch
        import torch_npu  # noqa: F401

        torch.set_num_threads(1)
        torch.npu.set_device(device)
        self.torch = torch
        self.device = "npu"
        self.geometry = geometry

    def prepare(self, page_tokens: int, capacity_pages: int):
        from sglang.srt.hardware_backend.npu.allocator_npu import NPUPagedTokenToKVPoolAllocator
        from sglang.srt.hardware_backend.npu.memory_pool_npu import NPUMHATokenToKVPool
        from sglang.srt.managers.cache_controller import HiCacheController
        from sglang.srt.mem_cache.memory_pool_host import MHATokenToKVPoolHost

        torch = self.torch
        pool = NPUMHATokenToKVPool(
            size=capacity_pages * page_tokens,
            page_size=page_tokens,
            dtype=torch.bfloat16,
            head_num=self.geometry["kv_heads"],
            head_dim=self.geometry["head_dim"],
            layer_num=self.geometry["layers"],
            device=self.device,
            enable_memory_saver=False,
        )
        pool.k_buffer_contiguous.fill_(1)
        pool.v_buffer_contiguous.fill_(2)
        allocator = NPUPagedTokenToKVPoolAllocator(
            pool.size, page_tokens, torch.bfloat16, self.device, pool, need_sort=False
        )
        host = MHATokenToKVPoolHost(pool, 1.0, 0, page_tokens, "page_first_direct")
        controller = HiCacheController(
            allocator, host, page_tokens, None, threading.Event(), io_backend="kernel_ascend"
        )
        torch.npu.synchronize()
        return controller

    def measure(self, controller, pages: int) -> dict:
        torch = self.torch
        host = controller.mem_pool_host
        allocator = controller.mem_pool_device_allocator
        host.clear()
        allocator.clear()
        indices = allocator.alloc(pages * controller.page_size)
        if indices is None:
            raise RuntimeError("The declared case exceeds the prepared device pool")
        torch.npu.synchronize()

        # This is the framework's actual index readback, measured separately from its enclosing write.
        host_indices = host.alloc(len(indices))
        begin = time.perf_counter_ns()
        controller.move_indices(host_indices, indices)
        index_ns = time.perf_counter_ns() - begin
        host.clear()

        begin = time.perf_counter_ns()
        selected = controller.write(indices)
        returned = time.perf_counter_ns()
        if selected is None or len(controller.ack_write_queue) != 1:
            raise RuntimeError("HiCache did not enqueue exactly one requested write")
        ack = controller.ack_write_queue.pop()
        ack.finish_event.synchronize()
        completed = time.perf_counter_ns()

        begin_clone = time.perf_counter_ns()
        copied_indices = selected.clone()
        clone_ns = time.perf_counter_ns() - begin_clone
        if not torch.equal(copied_indices, selected):
            raise RuntimeError("Host index clone changed the selected slots")
        for offset in range(0, len(selected), controller.page_size):
            data = host.get_data_page(int(selected[offset]), flat=False)
            if not bool(torch.all(data[0] == 1)) or not bool(torch.all(data[1] == 2)):
                raise RuntimeError("HiCache write did not publish the expected K/V page")

        begin_sync = time.perf_counter_ns()
        ack.finish_event.synchronize()
        idle_sync_ns = time.perf_counter_ns() - begin_sync
        return {
            "index_readback_ns": index_ns,
            "caller_return_ns": returned - begin,
            "completion_observed_ns": completed - begin,
            "idle_event_sync_ns": idle_sync_ns,
            "host_index_clone_ns": clone_ns,
            "physical_pages_verified": pages,
            "device_capacity_pages": allocator.size // controller.page_size,
            "host_capacity_pages": host.size // controller.page_size,
        }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--recipe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    recipe = read_json(args.recipe)
    reject_unknown(recipe, {"device", "geometry", "warmup", "repeats", "cases"}, "cache I/O recipe")
    device = recipe["device"]
    if type(device) is not int or device < 0:
        raise ValueError("device must be a nonnegative integer")
    geometry = recipe["geometry"]
    reject_unknown(geometry, {"layers", "kv_heads", "head_dim"}, "cache geometry")
    for key in ("layers", "kv_heads", "head_dim"):
        positive_integer(geometry[key], key)
    warmup = positive_integer(recipe["warmup"], "warmup")
    repeats = positive_integer(recipe["repeats"], "repeats")
    cases = recipe["cases"]
    if not isinstance(cases, list) or not cases:
        raise ValueError("cases must be a nonempty array")
    for case in cases:
        reject_unknown(case, {"page_tokens", "pages"}, "cache I/O case")
        for key in ("page_tokens", "pages"):
            positive_integer(case[key], key)

    args.output.mkdir(parents=True, exist_ok=False)
    sampler = CacheIOSampler(device, geometry)
    with (args.output / "measurements.jsonl").open("x", encoding="utf-8", buffering=1) as stream:
        for page_tokens in sorted({case["page_tokens"] for case in cases}):
            selected = [case for case in cases if case["page_tokens"] == page_tokens]
            controller = sampler.prepare(page_tokens, max(case["pages"] for case in selected))
            for case in selected:
                for ordinal in range(warmup + repeats):
                    measurement = sampler.measure(controller, case["pages"])
                    stream.write(
                        json.dumps({**case, "warmup": ordinal < warmup, "ordinal": ordinal, **measurement}) + "\n"
                    )
            del controller
            gc.collect()
            sampler.torch.npu.empty_cache()

    (args.output / "capture.json").write_text(
        json.dumps(
            {
                "status": "completed",
                "recipe": recipe,
                "torch": sampler.torch.__version__,
                "dtype": "bfloat16",
                "backend": "kernel_ascend",
                "host_layout": "page_first_direct",
                "scope": "Independent initialized idle single-device controller writes; no weights, trace, probe or target",
                "completion_boundary": "Write entry to finish-event synchronize return; includes the observing API",
                "limitations": "No competing I/O or CPU load; initialization and verification outside timing; idle sync is retained without subtraction",
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()
