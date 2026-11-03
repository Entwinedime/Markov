"""Keep native batch decisions while reporting the selected rows and allocations."""

from contextlib import contextmanager
from contextvars import ContextVar

from sglang.srt.managers.schedule_batch import ScheduleBatch
from sglang.srt.mem_cache import common


class NativeBatch(ScheduleBatch):
    selection_views: ContextVar[dict[int, str | None]] = ContextVar("native_selection_views")

    def prepare_for_extend(self):
        result = super().prepare_for_extend()
        self.token_to_kv_pool_allocator.peer.call("prepare_outputs")
        return result

    def prepare_for_decode(self):
        result = super().prepare_for_decode()
        self.token_to_kv_pool_allocator.peer.call("prepare_outputs")
        return result

    @staticmethod
    @contextmanager
    def report_selection(batch, view: str | None = "running"):
        if batch is None:
            yield
            return

        views = NativeBatch.selection_views
        token = views.set({**views.get({}), id(batch): view})
        try:
            yield
        finally:
            views.reset(token)

    def prepare_for_prebuilt(self):
        super().prepare_for_prebuilt()
        peer = self.token_to_kv_pool_allocator.peer
        self.control_work = dict(peer.context)
        peer.call("prebuilt", requests=[int(req.rid) for req in self.reqs])

    def filter_batch(self, *args, **kwargs):

        # PP can revisit an already empty last batch; acquisition excludes that no-work call too.
        fallback = "previous" if self.reqs and self.forward_mode.is_prebuilt() else None

        # Explicitly inactive scopes suppress idle cleanup; native prebuilt selection still has its own view.
        selection = self.selection_views.get({}).get(id(self), fallback) if self.reqs else None
        result = super().filter_batch(*args, **kwargs)
        if selection:

            # Report the native result, including retraction order; do not reproduce its selection policy.
            self.token_to_kv_pool_allocator.peer.call(
                "select", view=selection, requests=[int(req.rid) for req in self.reqs]
            )
        return result


class BatchAllocation:
    def __init__(self, schedule_batch) -> None:
        self.extend = schedule_batch.alloc_for_extend
        self.decode = schedule_batch.alloc_for_decode
        self.evict = common.evict_from_tree_cache
        self.preparation = None
        schedule_batch.alloc_for_extend = self.for_extend
        schedule_batch.alloc_for_decode = self.for_decode
        common.evict_from_tree_cache = self.before_eviction

    @contextmanager
    def allocation(self, batch, phase, tokens):
        self.preparation = batch, phase, tokens
        try:
            yield
        finally:
            self.preparation = None

    def before_eviction(self, tree_cache, num_tokens):
        if self.preparation is not None:
            batch, phase, tokens = self.preparation

            # Native input construction has finished; cache decisions and allocation follow this seam.
            batch.token_to_kv_pool_allocator.peer.call(
                "prepare_inputs",
                phase=phase,
                members=[
                    {"request": int(req.rid), "tokens": new_tokens}
                    for req, new_tokens in zip(batch.reqs, tokens, strict=True)
                ],
            )
        return self.evict(tree_cache, num_tokens)

    def for_extend(self, batch):
        with self.allocation(batch, "prefill", batch.extend_lens):
            return self.extend(batch)

    def for_decode(self, batch, token_per_req):
        with self.allocation(batch, "decode", [token_per_req] * len(batch.reqs)):
            return self.decode(batch, token_per_req)
