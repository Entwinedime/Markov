"""Observe actual input boundaries and tensor layouts without reading device data."""

import os

from markov.instrumentation.events import EventWriter


def implementation(value):
    return f"{type(value).__module__}.{type(value).__qualname__}"


class PreparationProbe:
    def __init__(self, writer: EventWriter):
        self.writer = writer
        self.device_name = None

    def hardware(self, device):
        if self.device_name is None:
            import torch

            self.device_name = torch.npu.get_device_name(device)
        return self.device_name

    def allocator_fields(self, allocator, sequence, last):
        return {
            "allocator_implementation": implementation(allocator),
            "device_name": self.hardware(sequence.device),
            "batch_size": sequence.shape[0],
            "sequence_dtype": str(sequence.dtype),
            "last_location_dtype": str(last.dtype),
            "free_page_dtype": str(allocator.free_pages.dtype),
            "free_page_stride_elements": allocator.free_pages.stride(0),
            "debug_memory_pool": allocator.debug_mode,
            "allocator_need_sort": allocator.need_sort,
        }

    def extend(
        self,
        original,
        allocator,
        prefix_lens,
        prefix_lens_cpu,
        seq_lens,
        seq_lens_cpu,
        last_loc,
        extend_num_tokens,
        num_new_pages=None,
    ):
        fields = self.allocator_fields(allocator, seq_lens, last_loc)
        queue = os.environ.get("TRITON_ENABLE_TASKQUEUE")
        fields.update(
            prefix_dtype=str(prefix_lens.dtype),
            triton_task_queue_enabled=None if queue is None else queue.lower() in ("true", "1"),
            supplied_new_pages=num_new_pages,
        )
        with self.writer.span("runtime.hicache.extend_allocator", fields):
            result = original(
                allocator,
                prefix_lens,
                prefix_lens_cpu,
                seq_lens,
                seq_lens_cpu,
                last_loc,
                extend_num_tokens,
                num_new_pages,
            )
            fields["allocated"] = result is not None
            return result

    def decode(self, original, allocator, seq_lens, seq_lens_cpu, last_loc):
        fields = self.allocator_fields(allocator, seq_lens, last_loc)
        with self.writer.span("runtime.hicache.decode_allocator", fields):
            result = original(allocator, seq_lens, seq_lens_cpu, last_loc)
            fields["allocated"] = result is not None
            return result

    def extend_table(self, original, batch):
        with self.writer.span("runtime.hicache.extend_allocation", {}):
            return original(batch)

    def decode_table(self, original, batch, token_per_req):
        fields = {
            "request_ids": [request.rid for request in batch.reqs],
            "sequence_lens_tokens": [length + token_per_req for length in batch.seq_lens_cpu.tolist()],
            "page_size_tokens": batch.tree_cache.page_size,
            "token_per_req": token_per_req,
        }
        with self.writer.span("runtime.hicache.decode_allocation", fields):
            return original(batch, token_per_req)

    def metadata(self, original, backend, forward_batch):
        table = backend.req_to_token_pool.req_to_token
        sequence, indices, extend = (
            forward_batch.seq_lens,
            forward_batch.req_pool_indices,
            forward_batch.extend_seq_lens,
        )
        fields = {
            "backend_implementation": implementation(backend),
            "route": "eager",
            "in_capture": False,
            "device_name": self.hardware(table.device),
            "max_context_tokens": backend.max_context_len,
            "context_tokens": forward_batch.seq_lens_sum,
            "batch_size": forward_batch.batch_size,
            "page_size_tokens": backend.page_size,
            "mode_is_target_verify": forward_batch.forward_mode.is_target_verify(),
            "mode_is_draft_extend": forward_batch.forward_mode.is_draft_extend_v2(),
            "sequence_shape": list(sequence.shape),
            "sequence_dtype": str(sequence.dtype),
            "sequence_device_type": sequence.device.type,
            "request_indices_shape": list(indices.shape),
            "request_indices_dtype": str(indices.dtype),
            "request_indices_device_type": indices.device.type,
            "request_table_shape": list(table.shape),
            "request_table_dtype": str(table.dtype),
            "request_table_contiguous": table.is_contiguous(),
            "extend_present": extend is not None,
            "speculative_present": forward_batch.spec_info is not None,
            "hybrid_swa": backend.is_hybrid_swa,
            "sliding_window_kv_pool": backend.use_sliding_window_kv_pool,
            "use_mla": backend.use_mla,
        }
        if extend is not None:
            fields.update(extend_dtype=str(extend.dtype), extend_device_type=extend.device.type)
        with self.writer.span("runtime.phase.attention_metadata", fields):
            return original(backend, forward_batch)

    def install(self):
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        bindings = {
            "sglang.srt.hardware_backend.npu.allocator_npu.NPUPagedTokenToKVPoolAllocator.alloc_extend": self.extend,
            "sglang.srt.hardware_backend.npu.allocator_npu.NPUPagedTokenToKVPoolAllocator.alloc_decode": self.decode,
            "sglang.srt.managers.schedule_batch.alloc_for_extend": self.extend_table,
            "sglang.srt.managers.schedule_batch.alloc_for_decode": self.decode_table,
            "sglang.srt.hardware_backend.npu.attention.ascend_backend.AscendAttnBackend.init_forward_metadata": self.metadata,
        }
        for target, observer in bindings.items():
            HookRegistry.register(target, observer, HookType.AROUND)
