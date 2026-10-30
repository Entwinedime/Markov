"""Observe request token ancestry at real cache lookup and commit boundaries."""

from __future__ import annotations

import os
from typing import Any, Callable

from markov.adapters.sglang.preparation_probe import implementation
from markov.instrumentation.events import EventWriter


class CacheProbe:
    def __init__(self, writer: EventWriter):
        self.writer = writer
        self.dictionary_index = 0

    def fields(self, request, tokens):
        self.dictionary_index += 1
        return {
            "request_id": request.rid,
            "token_dictionary": {
                "token_path_id": f"{os.getpid()}:{self.dictionary_index}",
                "token_ids": list(tokens),
            },
        }

    def lookup(self, original, cache, params):
        if params.req is None:
            return original(cache, params)
        fields = self.fields(params.req, params.key.raw_token_ids())
        with self.writer.span("hicache_cache_lookup_input_end", fields):
            return original(cache, params)

    def commit(self, original, cache, request, is_insert=True):
        # This is the unrounded, actually committed KV prefix. Target page
        # geometry must not inherit the source cache's page-aligned insertion.
        count = request._cache_commit_len()
        tokens = (request.origin_input_ids + request.output_ids)[:count]
        fields = self.fields(request, tokens)
        fields["insert_requested"] = is_insert and not cache.disable and not cache.disable_finished_insert
        fields["page_size_tokens"] = cache.page_size
        fields["cache_implementation"] = implementation(cache)
        with self.writer.span("hicache_cache_finished_req_lifecycle_commit_end", fields):
            return original(cache, request, is_insert)

    def unfinished(self, original, cache, req, chunked=False):
        fields = {
            "request_id": req.rid,
            "cache_implementation": implementation(cache),
            "page_size_tokens": cache.page_size,
            "input_tokens": len(req.get_fill_ids()),
            "protected_tokens_before": req.cache_protected_len,
            "chunked": chunked,
        }
        with self.writer.span("runtime.hicache.cache_unfinished", fields):
            result = original(cache, req, chunked)
            fields["protected_tokens_after"] = req.cache_protected_len
            return result

    def match_segments(self, original, cache, node, key):
        fields = {"cache_implementation": implementation(cache)}
        with self.writer.span("runtime.hicache.match_segments", fields):
            values, last_node = original(cache, node, key)
            fields.update(
                input_tensors=len(values),
                elements=sum(value.numel() for value in values),
                element_bytes=values[0].element_size() if values else 0,
                tensor_rank=values[0].dim() if values else 1,
                device_type=values[0].device.type if values else "none",
            )
            return values, last_node

    def prefetch_issue(
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
        with self.writer.span("runtime.hicache.file.prefetch_issue", fields):
            result = original(cache, req_id, last_host_node, new_input_tokens, last_hash, prefix_keys)
            pending = cache.ongoing_prefetch.get(req_id)
            operation = pending[3] if pending is not None else None
            fields["allocated_host_tokens"] = (
                len(operation.host_indices) if operation is not None and operation.host_indices is not None else None
            )
            return result

    def prefetch_progress(self, original: Callable, cache: Any, req_id: str) -> bool:
        if not cache.enable_storage or cache.cache_controller.storage_backend_type != "file":
            return original(cache, req_id)

        pending = cache.ongoing_prefetch.get(req_id)
        operation = pending[3] if pending is not None else None
        fields = {"request_id": req_id, "operation_present": operation is not None}
        with self.writer.span("runtime.hicache.file.prefetch_progress", fields):
            ready = original(cache, req_id)
            fields.update(
                ready=ready,
                completed_tokens=operation.completed_tokens if operation is not None else None,
                accepted_tokens=cache.prefetch_loaded_tokens_by_reqid.get(req_id),
            )
            return ready

    def file_read(
        self,
        original: Callable,
        controller: Any,
        operation: Any,
        hash_values: list[str],
        host_indices: Any,
        extra_info: Any = None,
    ) -> None:
        if controller.storage_backend_type != "file":
            return original(controller, operation, hash_values, host_indices, extra_info)

        fields = {
            "request_id": operation.request_id,
            "pages": len(hash_values),
            "page_tokens": controller.page_size,
            "completed_tokens_before": operation.completed_tokens,
        }
        with self.writer.span("runtime.hicache.file.read_batch", fields):
            result = original(controller, operation, hash_values, host_indices, extra_info)
            fields["completed_tokens_after"] = operation.completed_tokens
            return result

    def file_write(
        self, original: Callable, controller: Any, hash_values: list[str], host_indices: Any, extra_info: Any = None
    ) -> bool:
        if controller.storage_backend_type != "file":
            return original(controller, hash_values, host_indices, extra_info)

        fields = {"pages": len(hash_values), "page_tokens": controller.page_size}
        with self.writer.span("runtime.hicache.file.write_batch", fields):
            success = original(controller, hash_values, host_indices, extra_info)
            fields["success"] = success
            counts = controller.storage_backend.last_write_page_counts
            if counts is not None:
                fields.update(existing_pages=counts[0], new_pages=counts[1])
            return success

    def install(self):
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        for target in (
            "sglang.srt.mem_cache.radix_cache.RadixCache.match_prefix",
            "sglang.srt.mem_cache.hiradix_cache.HiRadixCache.match_prefix",
        ):
            HookRegistry.register(target, self.lookup, HookType.AROUND)
        for target in (
            "sglang.srt.mem_cache.radix_cache.RadixCache._match_prefix_helper",
            "sglang.srt.mem_cache.hiradix_cache.HiRadixCache._match_prefix_helper",
        ):
            HookRegistry.register(target, self.match_segments, HookType.AROUND)
        HookRegistry.register(
            "sglang.srt.mem_cache.radix_cache.RadixCache.cache_finished_req", self.commit, HookType.AROUND
        )
        HookRegistry.register(
            "sglang.srt.mem_cache.radix_cache.RadixCache.cache_unfinished_req", self.unfinished, HookType.AROUND
        )
        for target, wrapper in (
            ("sglang.srt.mem_cache.hiradix_cache.HiRadixCache.prefetch_from_storage", self.prefetch_issue),
            ("sglang.srt.mem_cache.hiradix_cache.HiRadixCache.check_prefetch_progress", self.prefetch_progress),
            ("sglang.srt.managers.cache_controller.HiCacheController._generic_page_get", self.file_read),
            ("sglang.srt.managers.cache_controller.HiCacheController._generic_page_set", self.file_write),
        ):
            HookRegistry.register(target, wrapper, HookType.AROUND)
