"""Native request bookkeeping over the core's content index and page ownership."""

import torch
from sglang.srt.mem_cache.base_prefix_cache import (
    DecLockRefResult,
    EvictResult,
    IncLockRefResult,
    InsertResult,
    MatchResult,
)
from sglang.srt.mem_cache.radix_cache import RadixCache
from sglang.srt.mem_cache.registry import default_radix_cache_factory


class PrefixCache(RadixCache):
    @staticmethod
    def build(context, *, expected):

        # Native chunk-only storage retains its request bookkeeping and never builds a content index.
        if context.disable_radix_cache and context.effective_chunked_prefill_size is not None:
            cache = default_radix_cache_factory(context)
            native_type = type(cache)
        else:
            cache = PrefixCache(context.params)
            native_type = RadixCache

        # The resource view replaces RadixCache storage, not the source implementation or policy.
        actual = {
            "type": f"{native_type.__module__}.{native_type.__qualname__}",
            "disabled": context.disable_radix_cache,
            "eviction_policy": context.params.eviction_policy.lower(),
        }
        if actual != expected:
            raise ValueError(f"native control cache differs from template implementation: {actual} != {expected}")
        return cache

    def __init__(self, params) -> None:
        if params.is_eagle or params.eviction_policy != "lru":
            raise ValueError("core prefix resources require ordinary token keys and LRU eviction")
        self.disable = params.disable
        self.req_to_token_pool = params.req_to_token_pool
        self.token_to_kv_pool_allocator = params.token_to_kv_pool_allocator
        self.peer = self.token_to_kv_pool_allocator.peer
        self.page_size = params.page_size
        self.disable_finished_insert = params.disable_finished_insert
        self.is_eagle = False
        self.device = torch.device("cpu")

        # Native request methods remain inherited; no native tree, allocator or eviction state is created.
        self.root_node = 0
        self._reset_session_radix_state()
        self._empty_match_result = MatchResult(torch.empty(0, dtype=torch.int64), 0, 0, 0)

    def reset(self):

        # Each simulation starts with its own resource pool; native flush must not create a second tree.
        raise NotImplementedError("reset prefix state by starting a new simulation resource pool")

    def key_tokens(self, key):
        if key.extra_key is not None:
            raise ValueError("request cache namespaces are outside the declared workload content contract")
        length = len(key) // self.page_size * self.page_size
        return list(key.token_ids[:length])

    def match_prefix(self, params):
        if self.disable:
            return self._empty_match_result
        result = self.peer.call("prefix", operation="match", tokens=self.key_tokens(params.key))

        # The core supplies the current binding, including splits; no mirrored native tree is maintained.
        indices = self.token_to_kv_pool_allocator.page_indices(result["pages"])
        node = result["node"]
        return MatchResult(indices, node, node, node)

    def insert(self, params):
        if self.disable:
            return InsertResult(prefix_len=0)
        tokens = self.key_tokens(params.key)
        indices = params.value[: len(tokens)]
        pages = (indices[:: self.page_size] // self.page_size - 1).tolist()
        result = self.peer.call("prefix", operation="insert", tokens=tokens, pages=pages)
        return InsertResult(prefix_len=result["existing_tokens"], last_device_node=result["node"])

    def inc_lock_ref(self, node):
        delta = 0 if self.disable else self.peer.call("prefix", operation="lock", node=node, acquire=True)
        return IncLockRefResult(delta=delta)

    def dec_lock_ref(self, node, params=None):
        delta = 0 if self.disable else self.peer.call("prefix", operation="lock", node=node, acquire=False)
        return DecLockRefResult(delta=delta)

    def evict(self, params):
        if self.disable:
            return EvictResult()
        evicted = 0
        for pages in self.peer.call("prefix", operation="evict", tokens=params.num_tokens):

            # Keep each native leaf release separate so allocator ordering remains native-visible.
            indices = self.token_to_kv_pool_allocator.page_indices(pages)
            self.token_to_kv_pool_allocator.free(indices)
            evicted += indices.numel()
        return EvictResult(num_tokens_evicted=evicted)

    def cache_unfinished_req(self, req, chunked=False):
        super().cache_unfinished_req(req, chunked=chunked)
        if not self.disable:
            self.peer.call("bind_prefix", request=int(req.rid), node=req.last_node, tokens=len(req.prefix_indices))

    def evictable_size(self):
        return 0 if self.disable else self.peer.call("capacity")["evictable_tokens"]

    def protected_size(self):
        return 0 if self.disable else self.peer.call("capacity")["protected_tokens"]
