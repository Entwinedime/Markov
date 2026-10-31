"""Keep native batch decisions while reporting the selected rows and allocations."""

from contextlib import contextmanager

from sglang.srt.managers.schedule_batch import ScheduleBatch


class NativeBatch(ScheduleBatch):
    control_selection = None

    @staticmethod
    @contextmanager
    def report_selection(batch, view="running"):
        if batch is None:
            yield
            return

        previous = getattr(batch, "control_selection", None)
        batch.control_selection = view
        try:
            yield
        finally:
            batch.control_selection = previous

    def prepare_for_prebuilt(self):
        super().prepare_for_prebuilt()
        peer = self.token_to_kv_pool_allocator.peer
        self.control_work = dict(peer.context)
        peer.call("prebuilt", requests=[int(req.rid) for req in self.reqs])

    def filter_batch(self, *args, **kwargs):

        # PP can revisit an already empty last batch; acquisition excludes that no-work call too.
        selection = self.control_selection if self.reqs else None
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
        schedule_batch.alloc_for_extend = self.for_extend
        schedule_batch.alloc_for_decode = self.for_decode

    def for_extend(self, batch):
        allocator = batch.token_to_kv_pool_allocator
        allocator.batch = batch
        allocator.phase = "prefill"
        allocator.tokens = batch.extend_lens
        return self.extend(batch)

    def for_decode(self, batch, token_per_req):
        allocator = batch.token_to_kv_pool_allocator
        allocator.batch = batch
        allocator.phase = "decode"
        allocator.tokens = [token_per_req] * len(batch.reqs)
        return self.decode(batch, token_per_req)
