"""Keep native batch decisions while reporting the selected rows and allocations."""

from sglang.srt.managers.schedule_batch import ScheduleBatch


class NativeBatch(ScheduleBatch):
    control_selection = False

    def prepare_for_prebuilt(self):
        super().prepare_for_prebuilt()
        peer = self.token_to_kv_pool_allocator.peer
        self.control_work = dict(peer.context)
        peer.call("prebuilt", requests=[int(req.rid) for req in self.reqs])

    def filter_batch(self, *args, **kwargs):
        result = super().filter_batch(*args, **kwargs)
        if self.control_selection:

            # Report the native result, including retraction order; do not reproduce its selection policy.
            self.token_to_kv_pool_allocator.peer.call("select", requests=[int(req.rid) for req in self.reqs])
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
