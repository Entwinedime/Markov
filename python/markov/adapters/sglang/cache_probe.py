"""Observe request token ancestry at real cache lookup and commit boundaries."""

import os

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
