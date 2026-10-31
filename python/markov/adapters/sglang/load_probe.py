"""Observe native load outcomes once for source facts and independent CPU costs."""

from dataclasses import dataclass
import time
from typing import Any, Callable

from markov.adapters.sglang.region_probe import IMPLEMENTATION
from markov.instrumentation.events import EventWriter


@dataclass
class LoadAttempt:
    failed_allocations: int = 0
    delegated_cpu_ns: int = 0
    backups: int = 0


class LoadProbe:
    def __init__(self, source: EventWriter | None, measurements: EventWriter | None) -> None:
        self.source = source
        self.measurements = measurements
        self.loading: LoadAttempt | None = None

    def init_load_back(self, original: Callable, cache: Any, params: Any) -> Any:
        if not params.best_match_node.evicted or params.req is None:
            return original(cache, params)

        pages, segments = 0, 0
        node = params.best_match_node
        while node.evicted:
            pages += len(node.host_value) // cache.page_size
            segments += 1
            node = node.parent
        attempt = LoadAttempt()
        self.loading = attempt
        begin, cpu = time.time_ns(), time.thread_time_ns()
        try:
            result = original(cache, params)
        finally:
            self.loading = None
        elapsed_cpu = time.thread_time_ns() - cpu
        end = time.time_ns()

        # Two failed native allocations distinguish capacity fallback from
        # threshold/quota skips. Only metadata is read, never tensor contents.
        if attempt.failed_allocations == 2 and len(result[0]) == 0:
            from sglang.srt.distributed.parallel_state import get_tensor_model_parallel_rank

            fields = {
                "implementation": IMPLEMENTATION,
                "phase": "prefill",
                "request_ids": [params.req.rid],
                "iteration": 0,
                "participant": get_tensor_model_parallel_rank(),
                "features": {"pages": pages, "segments": segments, "page_tokens": cache.page_size},
                "thread_cpu_ns": elapsed_cpu,
                "delegated_cpu_ns": attempt.delegated_cpu_ns,
                "successful_backups": attempt.backups,
                "status": "returned",
            }
            if self.source is not None:
                self.source.write("runtime.hicache.load_failed", begin, end, fields)
            if self.measurements is not None:
                self.measurements.write("region.load_failed", begin, end, fields)
        return result

    def load_back(self, original: Callable, cache: Any, node: Any, mem_quota: int | None = None) -> Any:
        if self.source is None:
            return original(cache, node, mem_quota)

        fields = {}
        with self.source.span("runtime.hicache.load_back", fields):
            result = original(cache, node, mem_quota)
            fields["loaded_tokens"] = result.numel() if result is not None else 0
            return result

    def cache_load(self, original: Callable, controller: Any, *args: Any, **kwargs: Any) -> Any:
        result = original(controller, *args, **kwargs)
        if self.loading is not None and result is None:
            self.loading.failed_allocations += 1
        return result

    def write_backup(self, original: Callable, cache: Any, node: Any, write_back: bool = False) -> int:
        if self.loading is None:
            return original(cache, node, write_back)

        cpu = time.thread_time_ns()
        result = original(cache, node, write_back)
        if result > 0:
            self.loading.delegated_cpu_ns += time.thread_time_ns() - cpu
            self.loading.backups += 1
        return result

    def writing_check(self, original: Callable, cache: Any, write_back: bool = False) -> Any:
        if self.loading is None or not write_back or self.loading.backups == 0:
            return original(cache, write_back)

        cpu = time.thread_time_ns()
        result = original(cache, write_back)
        self.loading.delegated_cpu_ns += time.thread_time_ns() - cpu
        return result

    def install(self) -> None:
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        bindings = {
            "sglang.srt.mem_cache.hiradix_cache.HiRadixCache.init_load_back": self.init_load_back,
            "sglang.srt.mem_cache.hiradix_cache.HiRadixCache.load_back": self.load_back,
            "sglang.srt.managers.cache_controller.HiCacheController.load": self.cache_load,
            "sglang.srt.mem_cache.hiradix_cache.HiRadixCache.write_backup": self.write_backup,
            "sglang.srt.mem_cache.hiradix_cache.HiRadixCache.writing_check": self.writing_check,
        }
        for target, observer in bindings.items():
            HookRegistry.register(target, observer, HookType.AROUND)
