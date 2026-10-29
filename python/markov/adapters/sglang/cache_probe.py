"""Observe request token ancestry at real cache lookup and commit boundaries."""

import os

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
        with self.writer.span("hicache_cache_finished_req_lifecycle_commit_end", fields):
            return original(cache, request, is_insert)

    def install(self):
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        for target in (
            "sglang.srt.mem_cache.radix_cache.RadixCache.match_prefix",
            "sglang.srt.mem_cache.hiradix_cache.HiRadixCache.match_prefix",
        ):
            HookRegistry.register(target, self.lookup, HookType.AROUND)
        HookRegistry.register(
            "sglang.srt.mem_cache.radix_cache.RadixCache.cache_finished_req", self.commit, HookType.AROUND
        )
