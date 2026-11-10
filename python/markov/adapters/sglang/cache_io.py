"""Measure real HiCache controller responses without model weights or a profiler."""

import argparse
import gc
import json
import os
from pathlib import Path
import threading
import time

from markov.artifacts.capture import capture_origin
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

    def prepare_indices(self, controller, pages: int):
        torch = self.torch
        host = controller.mem_pool_host
        allocator = controller.mem_pool_device_allocator
        host.clear()
        allocator.clear()
        controller.mem_pool_device.k_buffer_contiguous.fill_(1)
        controller.mem_pool_device.v_buffer_contiguous.fill_(2)
        indices = allocator.alloc(pages * controller.page_size)
        if indices is None:
            raise RuntimeError("The declared case exceeds the prepared device pool")
        torch.npu.synchronize()
        return indices

    def measure_backup(self, controller, pages: int) -> dict:
        torch = self.torch
        host = controller.mem_pool_host
        allocator = controller.mem_pool_device_allocator
        indices = self.prepare_indices(controller, pages)

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

    def measure_bandwidth(self, controller, pages: int, directory: Path) -> dict:
        from markov.adapters.sglang.file_io import FileIOSampler

        torch, host = self.torch, controller.mem_pool_host
        indices = self.prepare_indices(controller, pages)
        selected = host.alloc(len(indices))
        host_indices, device_indices = controller.move_indices(selected, indices)
        torch.npu.synchronize()

        start, end = torch.npu.Event(enable_timing=True), torch.npu.Event(enable_timing=True)
        with torch.npu.stream(controller.write_stream):
            start.record()
            host.backup_from_device_all_layer(
                controller.mem_pool_device, host_indices, device_indices, controller.io_backend
            )
            end.record()
        end.synchronize()
        backup_ns = round(start.elapsed_time(end) * 1e6)

        pool = controller.mem_pool_device
        pool.k_buffer_contiguous.zero_()
        pool.v_buffer_contiguous.zero_()
        torch.npu.synchronize()
        with torch.npu.stream(controller.load_stream):
            start.record()
            for layer in range(self.geometry["layers"]):
                host.load_to_device_per_layer(pool, host_indices, device_indices, layer, controller.io_backend)
            end.record()
        end.synchronize()
        load_ns = round(start.elapsed_time(end) * 1e6)
        values = indices.cpu()
        shape = (self.geometry["layers"], -1, self.geometry["kv_heads"], self.geometry["head_dim"])
        if not bool(torch.all(pool.k_buffer_contiguous.view(shape)[:, values] == 1)) or not bool(
            torch.all(pool.v_buffer_contiguous.view(shape)[:, values] == 2)
        ):
            raise RuntimeError("Independent data-path bandwidth copy changed KV values")

        file = FileIOSampler(self, controller, directory).measure(pages, measure_shared=True)
        return {
            "transfer_bytes": len(indices) * host.get_size_per_token(),
            "device_to_host_data_ns": backup_ns,
            "host_to_device_data_ns": load_ns,
            "host_to_storage_data_ns": sum(file["file_new_page_ns"]),
            "storage_to_host_data_ns": sum(file["file_read_page_ns"]),
            "storage_shared_data_ns": file["file_shared_ns"],
            "storage_shared_bytes": file["file_shared_bytes"],
            "physical_pages_verified": pages,
        }

    def measure_load(self, controller, pages: int, segments: int) -> dict:
        torch = self.torch
        host = controller.mem_pool_host
        allocator = controller.mem_pool_device_allocator
        indices = self.prepare_indices(controller, pages)
        selected = controller.write(indices)
        controller.ack_write_queue.pop().finish_event.synchronize()

        # Prepare valid host pages outside timing, then clear the device pool so
        # verification cannot accidentally pass using the original K/V values.
        allocator.clear()
        pool = controller.mem_pool_device
        pool.k_buffer_contiguous.zero_()
        pool.v_buffer_contiguous.zero_()
        widths = [pages // segments + (index < pages % segments) for index in range(segments)]
        parts = list(selected.split([count * controller.page_size for count in widths]))
        torch.npu.synchronize()

        begin = time.perf_counter_ns()
        combined = torch.cat(parts)
        device_indices = controller.load(combined)
        offset = 0
        cloned = []
        for part in parts:
            cloned.append(device_indices[offset : offset + len(part)].clone())
            offset += len(part)
        prepared = time.perf_counter_ns()

        begin_load = time.perf_counter_ns()
        producer = controller.start_loading()
        returned = time.perf_counter_ns()
        ack = controller.ack_load_queue.pop()
        ack.finish_event.synchronize()
        completed = time.perf_counter_ns()

        # MHA obtains K and V separately; both accesses enqueue a layer wait.
        counter = controller.layer_done_counter
        counter.set_consumer(-1)
        begin_consumer = time.perf_counter_ns()
        for layer in range(self.geometry["layers"]):
            counter.wait_until(layer)
            counter.wait_until(layer)
        inactive_consumer_ns = time.perf_counter_ns() - begin_consumer

        counter.set_consumer(producer)
        begin_consumer = time.perf_counter_ns()
        for layer in range(self.geometry["layers"]):
            counter.wait_until(layer)
            counter.wait_until(layer)
        active_consumer_ns = time.perf_counter_ns() - begin_consumer
        torch.npu.current_stream().synchronize()
        counter.set_consumer(-1)

        # The normal framework also keeps these index tensors alive through the
        # queued transfer. Synchronization and value checks are outside timing.
        restored = torch.cat(cloned)
        if not torch.equal(restored, device_indices):
            raise RuntimeError("Load index clones changed the allocated slots")
        values = restored.cpu()
        shape = (self.geometry["layers"], -1, self.geometry["kv_heads"], self.geometry["head_dim"])
        if not bool(torch.all(pool.k_buffer_contiguous.view(shape)[:, values] == 1)) or not bool(
            torch.all(pool.v_buffer_contiguous.view(shape)[:, values] == 2)
        ):
            raise RuntimeError("HiCache load did not restore the expected K/V pages")
        return {
            "load_prepare_return_ns": prepared - begin,
            "load_caller_return_ns": returned - begin_load,
            "load_completion_observed_ns": completed - begin_load,
            "load_consumer_active_ns": active_consumer_ns,
            "load_consumer_inactive_ns": inactive_consumer_ns,
            "load_consumer_increment_ns": active_consumer_ns - inactive_consumer_ns,
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
    reject_unknown(recipe, {"device", "geometry", "warmup", "repeats", "cases", "operations"}, "cache I/O recipe")
    device = recipe["device"]
    if type(device) is not int or device < 0:
        raise ValueError("device must be a nonnegative integer")
    geometry = recipe["geometry"]
    reject_unknown(geometry, {"layers", "kv_heads", "head_dim"}, "cache geometry")
    for key in ("layers", "kv_heads", "head_dim"):
        positive_integer(geometry[key], key)
    warmup = positive_integer(recipe["warmup"], "warmup")
    repeats = positive_integer(recipe["repeats"], "repeats")
    operations = recipe.get("operations", ["backup"])
    if (
        not isinstance(operations, list)
        or not operations
        or set(operations) - {"backup", "load", "file", "bandwidth"}
        or len(set(operations)) != len(operations)
    ):
        raise ValueError("operations must select backup, load, file, or bandwidth")
    cases = recipe["cases"]
    if not isinstance(cases, list) or not cases:
        raise ValueError("cases must be a nonempty array")
    for case in cases:
        reject_unknown(case, {"page_tokens", "pages", "segments"}, "cache I/O case")
        for key in ("page_tokens", "pages"):
            positive_integer(case[key], key)
        segments = positive_integer(case.get("segments", 1), "segments")
        if segments > case["pages"]:
            raise ValueError("Each load segment must contain at least one page")

    args.output.mkdir(parents=True, exist_ok=False)
    origin = capture_origin(args.output)
    os.environ["SGLANG_STEP_TIMING_DIR"] = ""
    sampler = CacheIOSampler(device, geometry)
    measurement_ports = set()
    with (args.output / "measurements.jsonl").open("x", encoding="utf-8", buffering=1) as stream:
        for page_tokens in sorted({case["page_tokens"] for case in cases}):
            selected = [case for case in cases if case["page_tokens"] == page_tokens]
            controller = sampler.prepare(page_tokens, max(case["pages"] for case in selected))
            for case in selected:
                for operation in operations:
                    for ordinal in range(warmup + repeats):
                        if operation == "backup":
                            measurement = sampler.measure_backup(controller, case["pages"])
                        elif operation == "load":
                            measurement = sampler.measure_load(controller, case["pages"], case.get("segments", 1))
                        elif operation == "bandwidth":
                            measurement = sampler.measure_bandwidth(controller, case["pages"], args.output)
                        else:
                            from markov.adapters.sglang.file_io import FileIOSampler

                            measurement = FileIOSampler(sampler, controller, args.output).measure(case["pages"])
                        measurement_ports.update(name for name in measurement if name.endswith("_ns"))
                        stream.write(
                            json.dumps(
                                {
                                    **case,
                                    "operation": operation,
                                    "warmup": ordinal < warmup,
                                    "ordinal": ordinal,
                                    **measurement,
                                }
                            )
                            + "\n"
                        )
            del controller
            gc.collect()
            sampler.torch.npu.empty_cache()

    (args.output / "capture.json").write_text(
        json.dumps(
            {
                "status": "completed",
                "capture_origin": origin,
                "recipe": recipe,
                "measurement_ports": sorted(measurement_ports),
                "torch": sampler.torch.__version__,
                "dtype": "bfloat16",
                "backend": "kernel_ascend",
                "host_layout": "page_first_direct",
                "scope": "Independent initialized idle controller/host/file I/O; no weights, profiler, snapshot or target",
                "completion_boundary": "Data-path NPU event intervals; file get/set data service and concurrent read/write, with initialization and value checks outside timing."
                if "bandwidth" in operations
                else "Device responses include finish-event observation. File responses cover native buffer allocation/read/publication and materialize/batch_set intervals.",
                "limitations": "Buffered warm-file service, not cold SSD or physical PCIe specifications; mixed file test uses two CPU workers."
                if "bandwidth" in operations
                else "No competing I/O or CPU load; initialization and verification outside timing; idle sync is retained without subtraction",
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()
