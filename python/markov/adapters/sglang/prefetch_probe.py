"""Observe file-prefetch control ports and closed background query groups."""

from contextlib import nullcontext
from contextvars import ContextVar
from dataclasses import dataclass
import time
from typing import Any, Callable

from markov.adapters.sglang.region_probe import IMPLEMENTATION
from markov.instrumentation.events import EventWriter


@dataclass
class Query:
    begin_ns: int
    fields: dict[str, Any]


@dataclass
class Progress:
    stopped_ns: int | None = None
    pages: int | None = None


class PrefetchProbe:
    def __init__(self, source: EventWriter | None, measurements: EventWriter | None) -> None:
        self.source = source
        self.measurements = measurements
        self.progress: Progress | None = None
        self.query: ContextVar[Query | None] = ContextVar("prefetch_query", default=None)

    def measure(self, name: str, request: str, page: int, begin_ns: int, end_ns: int, features: dict) -> None:
        if self.measurements is None:
            return

        from sglang.srt.distributed.parallel_state import (
            get_tensor_model_parallel_rank,
            get_tensor_model_parallel_world_size,
        )

        self.measurements.write(
            name,
            begin_ns,
            end_ns,
            {
                "implementation": IMPLEMENTATION,
                "phase": "prefill",
                "request_ids": [request],
                "iteration": 0,
                "participant": get_tensor_model_parallel_rank(),
                "participants": get_tensor_model_parallel_world_size(),
                "features": {**features, "page_tokens": page, "participants": get_tensor_model_parallel_world_size()},
                "status": "returned",
            },
        )

    def issue(
        self,
        original: Callable,
        cache: Any,
        req_id: str,
        last_host_node: Any,
        new_input_tokens: list[int],
        last_hash: str | None = None,
        prefix_keys: list[str] | None = None,
    ) -> None:
        if not cache.enable_storage or cache.cache_controller.storage_backend_type != "file":
            return original(cache, req_id, last_host_node, new_input_tokens, last_hash, prefix_keys)

        fields = {"request_id": req_id, "requested_tokens": len(new_input_tokens)}
        span = self.source.span("runtime.hicache.file.prefetch_issue", fields) if self.source else nullcontext()
        begin = time.time_ns()
        with span:
            result = original(cache, req_id, last_host_node, new_input_tokens, last_hash, prefix_keys)
            pending = cache.ongoing_prefetch.get(req_id)
            operation = pending[3] if pending is not None else None
            allocated = (
                len(operation.host_indices) if operation is not None and operation.host_indices is not None else 0
            )
            fields["allocated_host_tokens"] = allocated if operation is not None else None
            end = time.time_ns()

        self.measure(
            "prefetch.issue",
            req_id,
            cache.page_size,
            begin,
            end,
            {
                "candidate_pages": len(new_input_tokens) // cache.page_size,
                "allocated_pages": allocated // cache.page_size,
            },
        )
        return result

    def check(self, original: Callable, cache: Any, req_id: str) -> bool:
        if not cache.enable_storage or cache.cache_controller.storage_backend_type != "file":
            return original(cache, req_id)

        pending = cache.ongoing_prefetch.get(req_id)
        operation = pending[3] if pending is not None else None
        active = operation is not None and operation.host_indices is not None
        record = Progress()
        fields = {"request_id": req_id, "operation_present": operation is not None}
        span = self.source.span("runtime.hicache.file.prefetch_progress", fields) if self.source else nullcontext()
        begin = time.time_ns()
        self.progress = record
        try:
            with span:
                ready = original(cache, req_id)
                fields.update(
                    ready=ready,
                    completed_tokens=operation.completed_tokens if operation is not None else None,
                    accepted_tokens=cache.prefetch_loaded_tokens_by_reqid.get(req_id),
                )
                end = time.time_ns()
        finally:
            self.progress = None

        # Pending calls already belong to the measured idle-turn response.
        if ready:
            self.measure(
                "prefetch.check",
                req_id,
                cache.page_size,
                begin,
                record.stopped_ns if record.stopped_ns is not None else end,
                {
                    "operation_present": int(active),
                    "state_reductions": int(active and cache.prefetch_stop_policy != "best_effort"),
                },
            )
            if record.stopped_ns is not None:
                self.measure(
                    "prefetch.accept", req_id, cache.page_size, record.stopped_ns, end, {"pages": record.pages}
                )
        return ready

    def stop(self, original: Callable, controller: Any, operation: Any) -> Any:
        record = self.progress
        if record is not None:
            record.stopped_ns = time.time_ns()
        result = original(controller, operation)
        if record is not None:
            record.pages = result[0] // controller.page_size
        return result

    def storage_query(self, original: Callable, controller: Any, operation: Any) -> Any:
        if controller.storage_backend_type != "file":
            return original(controller, operation)

        from sglang.srt.managers.cache_controller import STORAGE_BATCH_SIZE

        begin = time.time_ns()
        queued = begin - (time.monotonic_ns() - round(operation.start_time * 1_000_000_000))
        pages = len(operation.token_ids) // controller.page_size
        fields = {
            "implementation": IMPLEMENTATION,
            "phase": "prefill",
            "request_ids": [operation.request_id],
            "iteration": 0,
            "participant": controller.tp_rank,
            "participants": controller.tp_size,
            "queued_at_ns": queued,
            "requested_pages": pages,
        }
        observed = Query(begin, fields)
        self.query.set(observed)
        result = original(controller, operation)
        hits = len(result[0])
        fields["hit_pages"] = hits
        fields["features"] = {
            "hashed_pages": pages
            if hits == pages
            else min(pages, (hits // STORAGE_BATCH_SIZE + 1) * STORAGE_BATCH_SIZE),
            "checked_pages": min(pages, hits + 1),
            "page_tokens": controller.page_size,
            "participants": controller.tp_size,
        }
        return result

    def reduce_query(self, original: Callable, controller: Any, tensor: Any, op: Any) -> Any:
        result = original(controller, tensor, op)
        observed = self.query.get()
        if observed is not None:
            self.query.set(None)
            self.measurements.write(
                "prefetch.query", observed.begin_ns, time.time_ns(), {**observed.fields, "status": "returned"}
            )
        return result

    def install(self) -> None:
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        HookRegistry.register(
            "sglang.srt.mem_cache.hiradix_cache.HiRadixCache.prefetch_from_storage", self.issue, HookType.AROUND
        )
        HookRegistry.register(
            "sglang.srt.mem_cache.hiradix_cache.HiRadixCache.check_prefetch_progress", self.check, HookType.AROUND
        )
        if self.measurements is None:
            return
        HookRegistry.register(
            "sglang.srt.managers.cache_controller.HiCacheController.terminate_prefetch", self.stop, HookType.AROUND
        )
        HookRegistry.register(
            "sglang.srt.managers.cache_controller.HiCacheController._storage_hit_query",
            self.storage_query,
            HookType.AROUND,
        )
        HookRegistry.register(
            "sglang.srt.managers.cache_controller.HiCacheController._all_reduce_prefetch_groups",
            self.reduce_query,
            HookType.AROUND,
        )
