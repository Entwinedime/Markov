"""Expose native PD lifecycle calls equally to observation and device-free control."""

from functools import partial

import torch
from torch.overrides import TorchFunctionMode

from markov_adapters.sglang.batches import BatchTensors


class MetadataCalls(TorchFunctionMode):
    def __init__(self, calls, queue) -> None:
        self.calls = calls
        self.buffer = queue.metadata_buffers.bootstrap_room
        self.storage = self.buffer.untyped_storage()._cdata
        self.requests = {entry.metadata_buffer_index: entry.req for entry in queue.queue}

    def __torch_function__(self, operation, types, args=(), kwargs=None):
        keywords = {} if kwargs is None else kwargs
        if operation is torch.Tensor.item and args[0].untyped_storage()._cdata == self.storage:
            tensor = args[0]
            row, column = divmod(tensor.storage_offset() - self.buffer.storage_offset(), self.buffer.stride(0))
            if column != 0:
                raise ValueError("native metadata gate reads outside the room field")
            return self.access("read", self.requests[row], operation, args, keywords)
        if operation is torch.Tensor.__setitem__ and args[0] is self.buffer:
            return self.access("write", self.requests[args[1]], operation, args, keywords)
        return operation(*args, **keywords)

    def access(self, direction, request, operation, arguments, keywords):
        work = self.calls.request_work(request)

        # The admission gate inspects current visibility; an ordinary data-read edge would turn it into a wait.
        if direction == "read":
            with self.calls.read("metadata", self.access, work) as query:
                result = operation(*arguments, **keywords)
                query["result"] = bool(result)
                return result

        # Observe the real scalar read or row reset; native code still decides when and how often it runs.
        with self.calls.call(f"metadata.{direction}", self.access, parameters={"members": 1}, work=work):
            self.calls.request_state.note("metadata", direction, [request])
            return operation(*arguments, **keywords)


class DisaggregatedCalls:
    def __init__(self, calls) -> None:
        self.calls = calls

    @staticmethod
    def create_sender(original, queue, req, num_kv_heads):
        result = original(queue, req, num_kv_heads)
        if result:
            req.disagg_kv_sender._markov_request = req
        return result

    @staticmethod
    def create_receiver(original, queue, req):
        result = original(queue, req)

        # The native queue owns this association; keep the same request through reservation and transfer.
        result.kv_receiver._markov_request = req
        return result

    def queue(self, operation, original, owner, *args, host_only=False, **kwargs):
        if not self.calls.enabled:
            return original(owner, *args, **kwargs)
        with self.calls.call(operation, original, parameters={}, work=self.calls.scheduler_work(), host_only=host_only) as frame:
            result = original(owner, *args, **kwargs)
            if operation == "prefill_finish":
                frame.outputs = self.calls.completed(result)
            return result

    def select(self, original, scheduler, *args, **kwargs):
        return self.calls.select(original, scheduler, *args, **kwargs)

    def poll(self, original, pollers, *args, **kwargs):
        calls = self.calls
        parent = calls.current.get()
        if not calls.enabled or (parent is not None and parent.operation == "protocol_poll"):
            return original(pollers, *args, **kwargs)

        work = calls.scheduler_work()
        work["context"]["poll_requests"] = [calls.request_id(poller._markov_request.rid) for poller in pollers]

        # TP/CP polling nests the single-group helper; one envelope retains both reductions without double counting.
        with calls.call("protocol_poll", original, parameters={}, work=work, host_only=True):
            return original(pollers, *args, **kwargs)

    def transferred(self, original, queue, *args, **kwargs):
        if not self.calls.enabled:
            return original(queue, *args, **kwargs)

        # The queue retains its entries through gate, commit and reset, so row ownership is stable for this call.
        with self.calls.call("decode_receive", original, parameters={}, work=self.calls.scheduler_work(), host_only=True), MetadataCalls(self.calls, queue):
            return original(queue, *args, **kwargs)

    def preallocate(self, original, queue, req, prefix_indices=None, prefix_len=None, total_prefix_len=None):
        if not self.calls.enabled:
            return original(queue, req, prefix_indices, prefix_len, total_prefix_len)
        prefix = 0 if prefix_len is None else prefix_len
        total = prefix if total_prefix_len is None else total_prefix_len
        fill = len(req.origin_input_ids) + max(len(req.output_ids) - 1, 0)
        parameters = {
            **BatchTensors.capacity(queue.token_to_kv_pool_allocator, queue.req_to_token_pool),
            "members": 1, "input_tokens": fill - total, "prefix_tokens": prefix, "total_prefix_tokens": total,
        }
        work = self.calls.request_work(req, total, fill)
        with self.calls.call(
            "preallocate", original, parameters=parameters, work=work,
            guards=(["lt", 0, "prefix_tokens"] if prefix else ["eq", "prefix_tokens", 0],),
        ):
            return original(queue, req, prefix_indices, prefix_len, total_prefix_len)

    def receive(self, original, queue, decode_req):
        if not self.calls.enabled:
            return original(queue, decode_req)
        req = decode_req.req
        parameters = {"members": 1}
        work = self.calls.request_work(req)
        with self.calls.request_state.receive(decode_req), self.calls.call("kv_commit", original, parameters=parameters, work=work):
            before = len(req.output_ids)
            result = original(queue, decode_req)
            self.calls.request_state.metadata_values("read", req, req.output_ids[-1] if len(req.output_ids) == before + 1 else None)
            return result

    def prepare(self, original, batch):
        if not self.calls.enabled:
            return original(batch)
        members = [
            {"id": req.rid, "context_tokens": len(req.prefix_indices), "new_tokens": req.extend_range.length}
            for req in batch.reqs
        ]
        work = self.calls.work(batch, members)
        parameters = BatchTensors.parameters(batch, sum(member["new_tokens"] for member in members))
        with self.calls.call("prebuilt_prepare", original, parameters=parameters, work=work, inputs=batch) as frame:
            result = original(batch)
            frame.tensor_outputs = BatchTensors.boundaries(batch)
        return result

    def batch(self, operation, original, batch, arguments, keywords):
        if not self.calls.enabled:
            return original(*arguments, **keywords)
        members = BatchTensors.members(batch)
        parameters = BatchTensors.parameters(batch, sum(member["new_tokens"] for member in members))
        with self.calls.request_state.members(batch), self.calls.call(operation, original, parameters=parameters, work=self.calls.work(batch, members)) as frame:
            result = original(*arguments, **keywords)
            if operation == "prebuilt_result":
                frame.outputs = self.calls.completed(batch.reqs)
            return result

    def commit(self, original, batch, *args, **kwargs):
        return self.batch("prebuilt_commit", original, batch, (batch, *args), kwargs)

    def result(self, original, processor, batch):
        return self.batch("prebuilt_result", original, batch, (processor, batch), {})

    def send(self, original, scheduler, req, last_chunk=False, end_idx=None):
        if not self.calls.enabled:
            return original(scheduler, req, last_chunk, end_idx)
        limit = min(req.extend_range.end, len(req.origin_input_ids)) if end_idx is None else end_idx
        parameters = {
            "chunk_start": req.start_send_idx, "chunk_limit": limit, "last_chunk": int(last_chunk),
            "page_size": scheduler.token_to_kv_pool_allocator.page_size,
        }
        with self.calls.call(
            "kv_prepare", original, parameters=parameters, work=self.calls.request_work(req), inputs=req,
            guards=(["eq", "last_chunk", int(last_chunk)],),
        ):
            return original(scheduler, req, last_chunk, end_idx)

    def install(self, mode):
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        scheduler = "sglang.srt.managers.scheduler.Scheduler."
        for name in ("poll_and_all_reduce", "poll_and_all_reduce_attn_cp_tp_group"):
            HookRegistry.register(f"sglang.srt.disaggregation.utils.{name}", self.poll, HookType.AROUND)

        if mode == "prefill":
            hooks = (
                ("sglang.srt.disaggregation.prefill.PrefillBootstrapQueue.create_sender", self.create_sender),

                # Preparation, cache, batch transforms and KV submission are separate child actions.
                (scheduler + "get_next_disagg_prefill_batch_to_run", partial(self.queue, "prefill_select", host_only=True)),
                (scheduler + "get_new_batch_prefill", self.select),
                (scheduler + "process_prefill_chunk", partial(self.queue, "prefill_chunk", host_only=True)),

                # Resource releases and publication are child actions; the remaining loop is CPU work.
                (scheduler + "process_disagg_prefill_inflight_queue", partial(self.queue, "prefill_finish", host_only=True)),
                (scheduler + "send_kv_chunk", self.send),
            )
        else:
            hooks = (
                ("sglang.srt.disaggregation.decode.DecodePreallocQueue._create_receiver_and_enqueue", self.create_receiver),
                (scheduler + "get_next_disagg_decode_batch_to_run", partial(self.queue, "decode_select", host_only=True)),
                (scheduler + "update_running_batch", self.select),
                (scheduler + "process_decode_queue", partial(self.queue, "decode_queue")),

                # Cache matching, tensor preparation and commit are child actions; admission only selects requests.
                (scheduler + "get_new_prebuilt_batch", partial(self.queue, "prebuilt_admit", host_only=True)),
                ("sglang.srt.disaggregation.decode.DecodePreallocQueue.pop_preallocated", partial(self.queue, "decode_reserve")),
                ("sglang.srt.disaggregation.decode.DecodeTransferQueue.pop_transferred", self.transferred),
                ("sglang.srt.disaggregation.decode.DecodePreallocQueue._pre_alloc", self.preallocate),
                ("sglang.srt.disaggregation.decode.DecodeTransferQueue._commit_transfer_to_req", self.receive),
                ("sglang.srt.managers.schedule_batch.ScheduleBatch.prepare_for_prebuilt", self.prepare),
                ("sglang.srt.managers.schedule_batch.ScheduleBatch.process_prebuilt", self.commit),
                ("sglang.srt.managers.scheduler_components.batch_result_processor.SchedulerBatchResultProcessor.process_batch_result_prebuilt",
                 self.result),
            )
        for target, hook in hooks:
            HookRegistry.register(target, hook, HookType.AROUND)
