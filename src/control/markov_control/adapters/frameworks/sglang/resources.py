"""Native-shaped metadata views backed by the core's unique slot and page ledger."""

from contextlib import contextmanager

import torch
from markov_adapters.sglang.allocation import AllocationABI
from markov_adapters.sglang.kv_layout import MhaKvLayout
from markov_control.peer import ControlPeer
from sglang.srt.mem_cache.allocator.base import BaseTokenToKVPoolAllocator


class AllocationDemand:
    def __init__(self) -> None:
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        for method, hook in (("alloc_for_extend", self.for_extend), ("alloc_for_decode", self.for_decode)):
            HookRegistry.register("sglang.srt.mem_cache.common." + method, hook, HookType.AROUND)
        HookRegistry.register(
            "sglang.srt.disaggregation.decode.DecodePreallocQueue._pre_alloc",
            self.preallocate, HookType.AROUND,
        )

    @staticmethod
    @contextmanager
    def demand(allocator, requests, tokens):
        allocator.demand = [
            {"request": int(req.rid), "tokens": count}
            for req, count in zip(requests, tokens, strict=True)
        ]
        try:
            yield
        finally:
            allocator.demand = None

    def for_extend(self, original, batch):
        with self.demand(batch.token_to_kv_pool_allocator, batch.reqs, batch.extend_lens):
            return original(batch)

    def for_decode(self, original, batch, token_per_req):
        with self.demand(batch.token_to_kv_pool_allocator, batch.reqs, [token_per_req] * len(batch.reqs)):
            return original(batch, token_per_req)

    def preallocate(self, original, queue, req, prefix_indices=None, prefix_len=None, total_prefix_len=None):
        prefix_len = 0 if prefix_len is None else prefix_len
        total_prefix_len = prefix_len if total_prefix_len is None else total_prefix_len
        if total_prefix_len != prefix_len:
            raise ValueError("Decode preallocation requires resident prefixes; tiered loadback is not implemented")
        tokens = len(req.origin_input_ids) + max(len(req.output_ids) - 1, 0) - total_prefix_len
        slots = queue.req_to_token_pool

        # Native preallocation assigns req.prefix_indices after allocating; use its call argument for slot binding.
        slots.prefix_bindings[int(req.rid)] = {"node": req.last_node, "tokens": prefix_len} if prefix_len else None
        try:
            with self.demand(queue.token_to_kv_pool_allocator, [req], [tokens]):
                return original(queue, req, prefix_indices, prefix_len, total_prefix_len)
        finally:
            del slots.prefix_bindings[int(req.rid)]


class RequestSlots:
    def __init__(self, peer: ControlPeer, size: int, context: int, extra: int) -> None:
        self.peer = peer
        self.prefix_bindings = {}
        self.size = size
        self.pre_alloc_size = extra
        self._alloc_size = size + extra + 1
        self.max_context_len = context
        self.device = "cpu"

        # This is the native tensor view of core allocation results, not an independent allocator.
        self.req_to_token = torch.zeros((self._alloc_size, context), dtype=torch.int32)

    def available_size(self):
        return self.peer.call("capacity", resource="slots")

    @property
    def free_slots(self):
        return [slot + 1 for slot in self.peer.call("free_slots")]

    def alloc(self, reqs):
        members = []
        for req in reqs:
            member = {"request": int(req.rid)}

            # The opaque core handle supplies cached pages; any uncached tail remains request-owned.
            prefix = self.prefix_bindings.get(
                int(req.rid),
                {"node": req.last_node, "tokens": len(req.prefix_indices)} if req.last_node is not None else None,
            )
            if prefix is not None:
                member["prefix"] = prefix
            members.append(member)
        bindings = self.peer.call("slots", members=members)
        for req, binding in zip(reqs, bindings, strict=True):
            req.req_pool_idx = binding["slot"] + 1
        return [req.req_pool_idx for req in reqs]

    def write(self, indices, values):
        self.req_to_token[indices] = values

    def free(self, req):
        self.peer.call("release_request", request=int(req.rid))
        req.req_pool_idx = None


class StateSlots:
    def __init__(self, size, *, peer, field):
        self.peer, self.field = peer, field
        self.size = size
        self.call("declare", capacity=size)

    def call(self, operation, **values):
        return self.peer.call("state_slots", field=self.field, operation=operation, **values)

    def available_size(self):
        return self.call("available")

    def alloc(self):
        return self.call("allocate")

    def bind(self, request, slot):
        self.call("bind", request=int(request.rid), slot=slot)

    def free(self, slot):
        self.call("release", slot=slot)


class KvStorage:
    def __init__(self, size: int, page_size: int, layout) -> None:
        self.size = size
        self.page_size = page_size
        self.buffers = None
        if layout is not None:
            descriptor = MhaKvLayout.read(layout)
            self.start_layer, self.end_layer = descriptor.start_layer, descriptor.end_layer
            self.head_num = descriptor.head_num
            self.buffers = descriptor.buffers(size, page_size)

    def get_contiguous_buf_infos(self):
        if self.buffers is None:
            raise ValueError("PD requires an observed KV buffer layout")
        return self.buffers.native_infos()

    def maybe_get_custom_mem_pool(self):
        return None


class PageAllocator:

    # Grouping is native control logic; only the final ownership change is delegated to the core.
    free_group_begin = BaseTokenToKVPoolAllocator.free_group_begin
    free_group_end = BaseTokenToKVPoolAllocator.free_group_end

    def __init__(self, peer: ControlPeer, size: int, page_size: int, layout, implementation: str) -> None:
        self.peer = peer
        self._markov_allocation = AllocationABI(implementation)
        self.size = size
        self.page_size = page_size
        self.device = "cpu"
        self.storage = KvStorage(size, page_size, layout)
        self.is_not_in_free_group = True
        self.free_group = []
        self.demand = None

    def available_size(self):
        return self.peer.call("capacity", resource="pages") * self.page_size

    def get_kvcache(self):
        return self.storage

    def reserve(self):
        allocation = self.peer.call("allocate", members=self.demand)
        if allocation["ordering"] is not None:
            self.merge_and_sort_free(allocation["ordering"])
        return allocation["members"]

    def merge_and_sort_free(self, ordering):

        # The core already ordered the pages; ResourceCalls exposes this native operation's execution graph.
        return None

    def allocate(self, dtype):
        return self.indices(self.reserve(), dtype)

    def indices(self, bindings, dtype):

        # SGLang reserves physical page zero; the core uses zero-based page identities.
        indices = [index + self.page_size for binding in bindings for index in binding["indices"]]
        return torch.tensor(indices, dtype=dtype)

    def page_indices(self, pages):
        starts = (torch.tensor(pages, dtype=torch.int64) + 1) * self.page_size
        return (starts[:, None] + torch.arange(self.page_size, dtype=torch.int64)).flatten()

    def alloc(self, need_size):
        return self.allocate(torch.int64)

    def alloc_extend(
        self, prefix_lens, prefix_lens_cpu, seq_lens, seq_lens_cpu, last_loc, extend_num_tokens, num_new_pages=None
    ):
        from sglang.srt.mem_cache.allocator import alloc_extend_naive

        # The declared NPU allocator returns int32 from its paged entry points, unlike alloc().
        bindings = self.reserve()
        result = self.indices(bindings, torch.int32)
        pages = [page + 1 for binding in bindings for page in binding["new_pages"]]
        if self._markov_allocation.extend_branch(len(pages)) == "extend_naive":

            # Replay only index computation over core-owned pages; the helper never allocates or frees them.
            free_pages = torch.tensor(pages, dtype=torch.int64)
            alloc_extend_naive(prefix_lens, seq_lens, last_loc, free_pages, result, self.page_size, "cpu")
        return result

    def alloc_decode(self, seq_lens, seq_lens_cpu, last_loc):
        return self.allocate(torch.int32)

    def free(self, indices):
        if indices.numel() == 0:
            return
        if not self.is_not_in_free_group:
            self.free_group.append(indices)
            return
        self.peer.call("release_pages", pages=torch.unique(indices // self.page_size - 1).tolist())
