"""Observe one closed background file query through its native rank reduction."""

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


class PrefetchProbe:
    def __init__(self, writer: EventWriter) -> None:
        self.writer = writer
        self.query: ContextVar[Query | None] = ContextVar("prefetch_query", default=None)

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
            self.writer.write(
                "prefetch.query", observed.begin_ns, time.time_ns(), {**observed.fields, "status": "returned"}
            )
        return result

    def install(self) -> None:
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

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
