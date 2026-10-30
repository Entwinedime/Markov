"""Record native file service and token ancestry only for explicit target diagnostics."""

from __future__ import annotations

from contextvars import ContextVar
from dataclasses import dataclass
import time
from typing import Any, Callable

from markov.instrumentation.events import EventWriter


@dataclass
class FileCall:
    begin_ns: int
    end_ns: int
    stage: str
    succeeded: bool


class FileBatch:
    def __init__(self, controller: Any, operation: Any, keys: list[str], writing: bool) -> None:
        self.participant = controller.tp_rank
        self.page_tokens = controller.page_size
        self.page_bytes = controller.mem_pool_host.get_size_per_token() * self.page_tokens
        self.pages = len(keys)
        self.offset = operation.hash_value.index(keys[0])
        self.request, self.iteration, self.tokens = operation._markov_storage_position
        self.anchor_tokens = len(self.tokens) - len(operation.token_ids)
        self.writing = writing
        self.calls: list[FileCall] = []
        self.publications: list[tuple[int, int]] = []
        self.stages: dict[str, int] = {}
        self.previous_recorder: Callable | None = None
        self.wall_begin_ns = time.time_ns()
        self.begin_ns = time.perf_counter_ns()

    def mark(self, stage: str, page: int | None = None, published: bool | None = None) -> None:
        now = time.perf_counter_ns()
        if stage == "read":
            self.stages[stage] = now
        elif stage == "publish":
            self.publications.append((page, now))

        if self.previous_recorder is not None:
            self.previous_recorder(stage, page, published)

    def report(self, writer: EventWriter, end_ns: int, succeeded: bool) -> None:
        fields: dict[str, Any] = {
            "status": "returned" if succeeded and all(call.succeeded for call in self.calls) else "incomplete",
            "request_id": self.request,
            "iteration": self.iteration,
            "participant": self.participant,
            "prefix_tokens": self.tokens,
            "page_bytes": self.page_bytes,
            "costs": [],
        }
        operation = "cache.storage_write." if self.writing else "cache.prefetch."
        fields["implementation"] = "file.materialize_then_batch_set" if self.writing else "file.generic_page_get"

        def add(
            stage: str, pages: int, page: int, begin: int, end: int, duration: int, contiguous: bool = True
        ) -> None:
            fields["costs"].append(
                {
                    "operation": operation + stage,
                    "pages": pages,
                    "end_token": self.anchor_tokens + (self.offset + page + 1) * self.page_tokens,
                    "begin_offset_ns": begin - self.begin_ns,
                    "end_offset_ns": end - self.begin_ns,
                    "duration_ns": duration,
                    "contiguous": contiguous,
                }
            )

        if fields["status"] == "returned":
            services = sum(call.end_ns - call.begin_ns for call in self.calls)
            if self.writing:
                boundary = self.stages["batch_set"]
                add("gather", self.pages, self.pages - 1, self.begin_ns, boundary, boundary - self.begin_ns)

                # The loop/return remainder is grouped once, without inventing an interval order.
                add("batch_prepare", self.pages, self.pages - 1, boundary, end_ns, end_ns - boundary - services, False)
                for page, call in enumerate(self.calls):
                    add(call.stage, 1, page, call.begin_ns, call.end_ns, call.end_ns - call.begin_ns)
            else:
                boundary = self.stages["read"]
                add(
                    "allocate",
                    self.pages,
                    self.pages - 1,
                    self.begin_ns,
                    boundary,
                    boundary - self.begin_ns - services,
                    False,
                )
                for page, call in enumerate(self.calls):
                    add("read_page", 1, page, call.begin_ns, call.end_ns, call.end_ns - call.begin_ns)
                previous = boundary
                for page, at in self.publications:
                    add("page_publish", 1, page, previous, at, at - previous)
                    previous = at
                add("return", self.pages, self.pages - 1, previous, end_ns, end_ns - previous)

        end_wall_ns = self.wall_begin_ns + end_ns - self.begin_ns
        writer.write(
            "storage.file.write" if self.writing else "storage.file.read", self.wall_begin_ns, end_wall_ns, fields
        )


class StorageProbe:
    def __init__(self, writer: EventWriter) -> None:
        self.writer = writer
        self.prefix: ContextVar[tuple[bool, list[int]] | None] = ContextVar("storage_prefix", default=None)
        self.backup: ContextVar[Any] = ContextVar("storage_backup", default=None)
        self.batch: ContextVar[FileBatch | None] = ContextVar("storage_batch", default=None)
        self.write_sequence = 0

    @staticmethod
    def tokens(node: Any) -> list[int]:
        segments = []
        while node.parent is not None:
            segments.append(node.key.raw_token_ids())
            node = node.parent
        return [token for segment in reversed(segments) for token in segment]

    def prefetch(
        self,
        original: Callable,
        cache: Any,
        req_id: str,
        last_host_node: Any,
        new_input_tokens: list[int],
        *args: Any,
        **kwargs: Any,
    ) -> Any:
        if not cache.enable_storage or cache.cache_controller.storage_backend_type != "file":
            return original(cache, req_id, last_host_node, new_input_tokens, *args, **kwargs)

        context = self.prefix.set((False, self.tokens(last_host_node)))
        try:
            return original(cache, req_id, last_host_node, new_input_tokens, *args, **kwargs)
        finally:
            self.prefix.reset(context)

    def write_backup(self, original: Callable, cache: Any, node: Any, *args: Any, **kwargs: Any) -> Any:
        context = self.prefix.set((True, self.tokens(node)))
        try:
            return original(cache, node, *args, **kwargs)
        finally:
            self.prefix.reset(context)

    def initialize(self, original: Callable, operation: Any, *args: Any, **kwargs: Any) -> None:
        original(operation, *args, **kwargs)
        position = self.prefix.get()
        if position is not None:
            writing, tokens = position
            if not writing:
                tokens = tokens + list(operation.token_ids.raw_token_ids())

            # Attach before the operation is put on a worker queue.
            iteration = self.write_sequence if writing else 0
            operation._markov_storage_position = (None if writing else operation.request_id, iteration, tokens)
            if writing:
                self.write_sequence += 1

    def page_backup(self, original: Callable, controller: Any, operation: Any) -> Any:
        context = self.backup.set(operation)
        try:
            return original(controller, operation)
        finally:
            self.backup.reset(context)

    def read(
        self, original: Callable, controller: Any, operation: Any, keys: list[str], indices: Any, extra: Any
    ) -> Any:
        if not hasattr(operation, "_markov_storage_position"):
            return original(controller, operation, keys, indices, extra)

        batch = FileBatch(controller, operation, keys, False)
        context = self.batch.set(batch)
        try:
            result = original(controller, operation, keys, indices, extra)
            end = time.perf_counter_ns()
        finally:
            self.batch.reset(context)
        batch.report(self.writer, end, "read" in batch.stages)
        return result

    def write(self, original: Callable, controller: Any, keys: list[str], indices: Any, extra: Any) -> bool:
        operation = self.backup.get()
        if operation is None or not hasattr(operation, "_markov_storage_position"):
            return original(controller, keys, indices, extra)

        batch = FileBatch(controller, operation, keys, True)
        context = self.batch.set(batch)
        try:
            result = original(controller, keys, indices, extra)
            end = time.perf_counter_ns()
        finally:
            self.batch.reset(context)
        batch.report(self.writer, end, result)
        return result

    def stage_recorder(self, original: Callable) -> Callable | None:
        previous = original()
        batch = self.batch.get()
        if batch is None:
            return previous

        batch.previous_recorder = previous
        return batch.mark

    def batch_set(self, original: Callable, backend: Any, *args: Any, **kwargs: Any) -> bool:
        batch = self.batch.get()
        if batch is not None:
            batch.stages["batch_set"] = time.perf_counter_ns()
        return original(backend, *args, **kwargs)

    def get(self, original: Callable, backend: Any, *args: Any, **kwargs: Any) -> Any:
        batch = self.batch.get()
        if batch is None:
            return original(backend, *args, **kwargs)

        begin = time.perf_counter_ns()
        result = original(backend, *args, **kwargs)
        end = time.perf_counter_ns()
        batch.calls.append(FileCall(begin, end, "read_page", result is not None))
        return result

    def set(self, original: Callable, backend: Any, *args: Any, **kwargs: Any) -> bool:
        batch = self.batch.get()
        if batch is None:
            return original(backend, *args, **kwargs)

        counts = backend._write_observation.active
        existing = counts[0]
        begin = time.perf_counter_ns()
        result = original(backend, *args, **kwargs)
        end = time.perf_counter_ns()
        stage = "existing_page" if counts[0] != existing else "new_page"
        batch.calls.append(FileCall(begin, end, stage, result))
        return result

    def install(self) -> None:
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        for target, wrapper in (
            ("sglang.srt.mem_cache.hiradix_cache.HiRadixCache.prefetch_from_storage", self.prefetch),
            ("sglang.srt.mem_cache.hiradix_cache.HiRadixCache.write_backup_storage", self.write_backup),
            ("sglang.srt.managers.cache_controller.StorageOperation.__init__", self.initialize),
            ("sglang.srt.managers.cache_controller.HiCacheController._page_backup", self.page_backup),
            ("sglang.srt.utils.step_timing.storage_read_stage_recorder", self.stage_recorder),
            ("sglang.srt.mem_cache.hicache_storage.HiCacheFile.batch_set", self.batch_set),
            ("sglang.srt.mem_cache.hicache_storage.HiCacheFile.get", self.get),
            ("sglang.srt.mem_cache.hicache_storage.HiCacheFile.set", self.set),
        ):
            HookRegistry.register(target, wrapper, HookType.AROUND)
