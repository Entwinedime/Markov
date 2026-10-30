"""Native-shaped metadata views backed by the core's unique slot and page ledger."""

import torch
from markov_control.peer import ControlPeer


class RequestSlots:
    def __init__(self, peer: ControlPeer, size: int, context: int, extra: int) -> None:
        self.peer = peer
        self.size = size
        self.pre_alloc_size = extra
        self._alloc_size = size + extra + 1
        peer.call("declare_slots", slots=size + extra)
        self.max_context_len = context
        self.device = "cpu"

        # This is the native tensor view of core allocation results, not an independent allocator.
        self.req_to_token = torch.zeros((self._alloc_size, context), dtype=torch.int32)

    def available_size(self):
        return self.peer.call("capacity")["slots"]

    @property
    def free_slots(self):
        return [slot + 1 for slot in self.peer.call("free_slots")]

    def alloc(self, reqs):
        members = []
        for req in reqs:
            member = {"request": int(req.rid)}

            # The opaque core handle supplies cached pages; any uncached tail remains request-owned.
            if req.last_node is not None:
                member["prefix"] = {"node": req.last_node, "tokens": len(req.prefix_indices)}
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


class KvStorage:
    def __init__(self, size: int, page_size: int) -> None:
        self.size = size
        self.page_size = page_size

    def maybe_get_custom_mem_pool(self):
        return None


class ChunkIndices:
    def __init__(self, cache, peer: ControlPeer) -> None:
        self.native_retain = cache.cache_unfinished_req
        self.trace = getattr(cache, "trace", None)
        self.peer = peer
        cache.cache_unfinished_req = self.retain

    def retain(self, request, chunked=False):
        result = self.native_retain(request, chunked=chunked)

        # Preserve native call timing; a saved chunk is consumed by the following scheduling decision.
        if chunked and self.trace is None:
            self.peer.call("retain", request=int(request.rid))
        return result


class PageAllocator:
    def __init__(self, peer: ControlPeer, size: int, page_size: int) -> None:
        self.peer = peer
        self.size = size
        self.page_size = page_size
        self.device = "cpu"
        self.storage = KvStorage(size, page_size)
        self.batch = None
        self.phase = None
        self.tokens = []
        self.group = None
        self.reservation = None
        self.trace = None

    def available_size(self):
        return self.peer.call("capacity")["pages"] * self.page_size

    def get_kvcache(self):
        return self.storage

    def allocate(self):
        if self.reservation is None:
            members = [
                {"request": int(req.rid), "tokens": tokens}
                for req, tokens in zip(self.batch.reqs, self.tokens, strict=True)
            ]
            bindings = self.peer.call("allocate", phase=self.phase, members=members)
        else:
            req = self.reservation
            bindings = self.peer.call(
                "reserve_pages", phase="prefill", members=[{"request": int(req.rid), "tokens": req.kv_allocated_len}]
            )

        # SGLang reserves physical page zero; core identities start at zero.
        indices = [index + self.page_size for binding in bindings for index in binding["indices"]]
        return torch.tensor(indices, dtype=torch.int64)

    def alloc(self, need_size):
        return self.allocate()

    def alloc_extend(
        self, prefix_lens, prefix_lens_cpu, seq_lens, seq_lens_cpu, last_loc, extend_num_tokens, num_new_pages=None
    ):
        return self.allocate()

    def alloc_decode(self, seq_lens, seq_lens_cpu, last_loc):
        return self.allocate()

    def free(self, indices):
        if self.trace is not None:
            with self.trace.metadata():
                pages = torch.unique(indices // self.page_size - 1).tolist()
            pending = sum(tensor.numel() for tensor in self.group) if self.group is not None else 0
            mode = "immediate" if self.group is None else "deferred"
            self.trace.release([indices], pages, mode, pending)
            if self.group is not None and indices.numel() != 0:
                self.group.append(indices)
            return
        if indices.numel() == 0:
            return
        if self.group is not None:
            self.group.append(indices)
            return
        pages = torch.unique(indices // self.page_size - 1).tolist()
        self.peer.call("release_pages", pages=pages)

    def free_group_begin(self):
        self.group = []

    def free_group_end(self):
        pending = self.group
        self.group = None
        if pending:
            if self.trace is None:
                self.free(torch.cat(pending))
            else:
                pages = torch.unique(torch.cat(pending) // self.page_size - 1).tolist()
                self.trace.release(pending, pages, "group")
                self.peer.call("release_pages", pages=pages)
