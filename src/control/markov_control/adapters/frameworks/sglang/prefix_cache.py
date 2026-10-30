"""Native request bookkeeping over the core's content index and page ownership."""

import torch
from markov_control.adapters.frameworks.sglang.index_work import IndexTrace
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
    def build(context):

        # Native chunk-only storage retains its request bookkeeping and never builds a content index.
        if context.disable_radix_cache and context.effective_chunked_prefill_size is not None:
            return default_radix_cache_factory(context)
        return PrefixCache(context.params)

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
        self.node_indices = {}
        self._reset_session_radix_state()
        self._empty_match_result = MatchResult(torch.empty(0, dtype=torch.int64), 0, 0, 0)
        self.trace = (
            None
            if self.disable
            else IndexTrace(self.peer, self.req_to_token_pool.req_to_token, self._empty_match_result.device_indices)
        )
        self.token_to_kv_pool_allocator.trace = self.trace

    def reset(self):

        # Each simulation starts with its own resource pool; native flush must not create a second tree.
        raise NotImplementedError("reset prefix state by starting a new simulation resource pool")

    def key_tokens(self, key):
        if key.extra_key is not None:
            raise ValueError("request cache namespaces are outside the declared workload content contract")
        length = len(key) // self.page_size * self.page_size
        return list(key.token_ids[:length])

    def split_indices(self, step):
        prefix = step["split_prefix"]
        if prefix is not None:
            node, length = step["node"], step["matched_tokens"]
            indices = self.node_indices[node]

            # Reproduce only native tensor work; the core already chose the split and owns both bindings.
            with self.trace.service("split"):
                self.node_indices[prefix] = indices[:length].clone()
                self.node_indices[node] = indices[length:].clone()
            return prefix
        return step["node"]

    def match_prefix(self, params):
        if self.disable:
            return self._empty_match_result
        result = self.peer.call("prefix", operation="match", tokens=self.key_tokens(params.key))
        with self.trace.service("match" if result["path"] else "empty_match"):
            segments = [self.node_indices[self.split_indices(step)] for step in result["path"]]
            indices = torch.cat(segments) if segments else self._empty_match_result.device_indices
            node = result["node"]
            if params.req is not None:
                self.trace.effects["prefix"] = {"request": int(params.req.rid), "indices": self.trace.value(indices)}
        return MatchResult(indices, node, node, node)

    def insert(self, params):
        if self.disable:
            return InsertResult(prefix_len=0)
        with self.trace.service("insert"):
            tokens = self.key_tokens(params.key)
            indices = params.value[: len(tokens)]
            with self.trace.metadata():
                pages = (indices[:: self.page_size] // self.page_size - 1).tolist()
            result = self.peer.call("prefix", operation="insert", tokens=tokens, pages=pages)
            with self.trace.service("insert_path"):
                for step in result["path"]:
                    indices = indices[step["matched_tokens"] :]
                    self.split_indices(step)
                if result["inserted"]:
                    self.node_indices[result["node"]] = indices.clone()
        return InsertResult(prefix_len=result["existing_tokens"], last_device_node=result["node"])

    def inc_lock_ref(self, node):
        if self.disable:
            return IncLockRefResult(delta=0)
        with self.trace.service("acquire"):
            delta = self.peer.call("prefix", operation="lock", node=node, acquire=True)
        return IncLockRefResult(delta=delta)

    def dec_lock_ref(self, node, params=None):
        if self.disable:
            return DecLockRefResult(delta=0)
        with self.trace.service("release"):
            delta = self.peer.call("prefix", operation="lock", node=node, acquire=False)
        return DecLockRefResult(delta=delta)

    def evict(self, params):
        if self.disable:
            return EvictResult()
        evicted = 0
        with self.trace.service("evict"):
            nodes = self.peer.call("prefix", operation="evict", tokens=params.num_tokens)
            for node in nodes:
                indices = self.node_indices.pop(node)
                evicted += indices.numel()

                # Preserve compressed-leaf free calls and order; execution feedback commits the pool update.
                self.token_to_kv_pool_allocator.free(indices)
        return EvictResult(num_tokens_evicted=evicted)

    def cache_unfinished_req(self, req, chunked=False):
        if self.disable:
            return super().cache_unfinished_req(req, chunked=chunked)
        with self.trace.service("retain_request", req):
            super().cache_unfinished_req(req, chunked=chunked)
            self.trace.effects["binding"] = {
                "request": int(req.rid),
                "node": req.last_node,
                "tokens": len(req.prefix_indices),
            }
            self.trace.prefix(req)

    def cache_finished_req(self, req, is_insert=True):
        if self.disable:
            return super().cache_finished_req(req, is_insert=is_insert)
        with self.trace.service("release_request", req):
            self.trace.effects["release_request"] = int(req.rid)
            super().cache_finished_req(req, is_insert=is_insert)

    def evictable_size(self):
        return 0 if self.disable else self.peer.call("capacity")["evictable_tokens"]

    def protected_size(self):
        return 0 if self.disable else self.peer.call("capacity")["protected_tokens"]
