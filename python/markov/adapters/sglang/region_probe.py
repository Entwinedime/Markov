"""Measure eager semantic regions, without native hooks or profiler exports."""

from __future__ import annotations

import time
from dataclasses import dataclass
from typing import Any, Callable

from markov.instrumentation.events import EventWriter


IMPLEMENTATION = "sglang.ascend.eager"
IDLE_SAMPLE_LIMIT = 128  # Bound optional observations, never native execution.


@dataclass(frozen=True)
class IdlePoll:
    request: str
    begin_ns: int
    timer_ns: int


class RegionProbe:
    def __init__(self, writer: EventWriter, *, record_scheduler: bool = False) -> None:
        self.writer = writer
        self.running = None
        self.forward_event = None
        self.preparations = {}
        self.seen_preparations = set()
        self.preparing = None
        self.seen_phases = set()
        self.results = {}
        self.pending = []
        self.processing = None
        self.iterations = {}
        self.record_scheduler = record_scheduler
        self.poll_scheduler = None
        self.previous_poll: IdlePoll | None = None
        self.idle_intervals: list[tuple[IdlePoll, int]] = []
        self.idle_sample_count = 0
        self.controller_begin = None
        self.frontier = None
        self.broadcast = None

    def receive_turn(self, original, receiver, *args, **kwargs):
        self.broadcast = None
        result = original(receiver, *args, **kwargs)
        self.controller_begin = self.broadcast[1] if self.broadcast is not None else time.time_ns()
        return result

    def broadcast_requests(self, original, receiver, requests):
        begin = time.time_ns()
        result = original(receiver, requests)
        end = time.time_ns()
        if receiver.ps.tp_size > 1:
            tokens = sum(len(item.input_ids) for item in result if hasattr(item, "input_ids"))
            self.broadcast = (begin, end, {"requests": len(result), "input_tokens": tokens})
        return result

    def record_control(self, part, begin, end, fields):
        if begin is not None:
            self.writer.write("region.control." + part, begin, end, {**fields, "status": "returned"})

    def select_batch(self, original: Callable, scheduler: Any) -> Any:
        if self.idle_sample_count >= IDLE_SAMPLE_LIMIT and not self.idle_intervals:
            return original(scheduler)

        self.poll_scheduler = scheduler
        try:
            batch = original(scheduler)
        finally:
            self.poll_scheduler = None

        # Buffer observations while polling: writing each turn would change
        # the very cadence being measured. Execution is never truncated.
        if batch is not None and self.idle_intervals:
            from sglang.srt.distributed.parallel_state import get_tensor_model_parallel_world_size

            for prior, duration in self.idle_intervals:
                self.writer.write(
                    "region.scheduler_idle",
                    prior.begin_ns,
                    prior.begin_ns + duration,
                    {
                        "implementation": IMPLEMENTATION,
                        "phase": "prefill",
                        "request_ids": [prior.request],
                        "features": {"participants": get_tensor_model_parallel_world_size()},
                        "idle_interval_ns": duration,
                        "status": "returned",
                    },
                )
            self.idle_intervals.clear()
        return batch

    def prefetch_progress(self, original: Callable, cache: Any, req_id: str) -> bool:
        scheduler = self.poll_scheduler
        if scheduler is None or not cache.enable_storage or cache.cache_controller.storage_backend_type != "file":
            return original(cache, req_id)

        idle = scheduler.last_batch is None and scheduler.running_batch.is_empty()
        begin, timer = time.time_ns(), time.perf_counter_ns()
        prior = self.previous_poll
        if idle and prior is not None and prior.request == req_id and self.idle_sample_count < IDLE_SAMPLE_LIMIT:
            self.idle_intervals.append((prior, timer - prior.timer_ns))
            self.idle_sample_count += 1

        ready = original(cache, req_id)
        self.previous_poll = IdlePoll(req_id, begin, timer) if idle and not ready else None
        return ready

    @staticmethod
    def variables(batch) -> dict:
        phase = "decode" if batch.forward_mode.is_decode() else "prefill"
        contexts = batch.seq_lens_cpu.tolist()
        queries = [1] * len(contexts) if phase == "decode" else batch.extend_lens
        return {
            "phase": phase,
            "request_ids": [request.rid for request in batch.reqs],
            "features": {
                "query_tokens": sum(queries),
                "context_tokens": sum(contexts),
                "attention_pairs": sum(q * (2 * c - q + 1) / 2 for q, c in zip(queries, contexts)),
                "batch_size": len(contexts),
                "page_tokens": batch.tree_cache.page_size,
            },
        }

    def prepare(self, original, batch):
        control_end = time.time_ns()
        import torch
        from sglang.srt.distributed.parallel_state import get_tensor_model_parallel_rank

        fields = {"implementation": IMPLEMENTATION, "preparation_keys": [], "new_preparations": 0}
        start = torch.npu.Event(enable_timing=True)
        end = torch.npu.Event(enable_timing=True)
        begin = time.time_ns()
        start.record()
        self.preparing = fields
        try:
            with self.writer.span("region.prepare", fields):
                result = original(batch)
                fields.update(self.variables(batch))
                identity = tuple(fields["request_ids"])
                fields["participant"] = get_tensor_model_parallel_rank()
                fields["iteration"] = self.iterations.get(identity, 0)
                self.iterations[identity] = fields["iteration"] + 1
                phase, features = fields["phase"], fields["features"]
                features.update(
                    first_phase=int(phase not in self.seen_phases),
                    new_preparations=fields["new_preparations"] if phase in self.seen_phases else 0,
                )
                self.seen_phases.add(phase)
        finally:
            self.preparing = None
        end.record()
        self.pending.append(("region.prepare_stream", start, end, begin, fields))
        self.preparations[id(batch)] = fields
        self.record_control("before_prepare", self.controller_begin, control_end, fields)
        if self.broadcast is not None:
            first, last, features = self.broadcast
            self.record_control("broadcast", first, last, {**fields, "features": {**features, "page_tokens": fields["features"]["page_tokens"]}})
        self.frontier = time.time_ns()
        return result

    def allocate_extend(
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
        from sglang.srt.utils import get_num_new_pages, next_power_of_2

        pages = num_new_pages
        if pages is None:
            pages = get_num_new_pages(seq_lens_cpu, allocator.page_size, prefix_lens_cpu)
        key = None

        # The fused allocator's constexpr bounds and pointer alignment determine
        # its specialization. The native implementation switches at 200 pages.
        if pages < 200 and self.preparing is not None:
            key = (
                next_power_of_2(prefix_lens.shape[0]),
                allocator.page_size,
                next_power_of_2(extend_num_tokens),
                allocator.free_pages.data_ptr() % 16,
            )
        result = original(
            allocator, prefix_lens, prefix_lens_cpu, seq_lens, seq_lens_cpu, last_loc, extend_num_tokens, num_new_pages
        )
        if key is not None and result is not None:
            self.preparing["preparation_keys"].append(key)
            self.preparing["new_preparations"] += int(key not in self.seen_preparations)
            self.seen_preparations.add(key)
        return result

    def run_batch(self, original, scheduler, batch, *args, **kwargs):
        control_end, frontier = time.time_ns(), self.frontier
        if not (batch.forward_mode.is_decode() or batch.forward_mode.is_extend()):
            return original(scheduler, batch, *args, **kwargs)

        fields = {**self.preparations.pop(id(batch)), "implementation": IMPLEMENTATION}
        self.running = fields
        self.forward_event = None
        try:
            with self.writer.span("region.batch", fields):
                result = original(scheduler, batch, *args, **kwargs)
            start, end, begin, stream = self.forward_event
            end.record(stream)
            self.pending.append(("region.stream", start, end, begin, fields))
            self.results[id(result)] = fields
            self.record_control("before_batch", frontier, control_end, fields)
            return result
        finally:
            self.running = None
            self.forward_event = None
            self.frontier = time.time_ns()

    def forward(self, original: Callable, worker: Any, *args: Any, **kwargs: Any) -> Any:
        if self.running is None:
            return original(worker, *args, **kwargs)

        import torch

        # The forward stream has already waited for scheduler inputs. Its end
        # is recorded after run_batch also enqueues token relay and CPU copy.
        start = torch.npu.Event(enable_timing=True)
        end = torch.npu.Event(enable_timing=True)
        stream = torch.npu.current_stream()
        self.forward_event = (start, end, time.time_ns(), stream)
        start.record(stream)
        result = original(worker, *args, **kwargs)

        # The worker has now applied the actual batch's consumer index.
        counter = worker.hicache_layer_transfer_counter
        self.running["load_consumer_active"] = counter is not None and counter.consumer_index >= 0
        return result

    def input_metadata(self, original: Callable, backend: Any, batch: Any) -> Any:
        fields = self.running
        if fields is None:
            return original(backend, batch)

        import torch

        start = torch.npu.Event(enable_timing=True)
        end = torch.npu.Event(enable_timing=True)
        stream = torch.npu.current_stream()
        begin = time.time_ns()
        start.record(stream)
        wall_begin = time.perf_counter_ns()
        cpu_begin = time.thread_time_ns()
        try:
            return original(backend, batch)
        finally:
            cpu_ns = time.thread_time_ns() - cpu_begin
            wall_ns = time.perf_counter_ns() - wall_begin
            fields["input_metadata"] = {"wall_ns": wall_ns, "thread_cpu_ns": cpu_ns}
            end.record(stream)
            self.pending.append(("input_metadata", start, end, begin, fields))

    def process_result(self, original, scheduler, batch, result):
        control_end, frontier = time.time_ns(), self.frontier
        fields = self.results.pop(id(result), None)
        if fields is None:
            return original(scheduler, batch, result)

        fields = {**fields, "features": {**fields["features"]}}
        span = self.writer.span("region.result", fields)
        self.processing = (span, fields)
        try:
            with span:
                value = original(scheduler, batch, result)
                fields["features"]["finished"] = sum(request.finished() for request in batch.reqs)
        finally:
            self.processing = None

        # Results already wait for token copies. Querying completed events adds
        # no synchronization to the measured forward or preparation paths.
        pending = []
        for name, start, end, begin, measured in self.pending:
            if end.query():
                duration_ns = round(start.elapsed_time(end) * 1_000_000)
                if name == "input_metadata":
                    measured["input_metadata"]["stream_interval_ns"] = duration_ns
                else:
                    self.writer.write(name, begin, begin, {**measured, "stream_interval_ns": duration_ns})
            else:
                pending.append((name, start, end, begin, measured))
        self.pending = pending
        self.record_control("before_result", frontier, control_end, fields)
        self.frontier = time.time_ns()
        return value

    def send(self, original, socket, message, *args, **kwargs):
        from sglang.srt.managers.io_struct import BatchTokenIDOutput

        value = original(socket, message, *args, **kwargs)
        if self.processing is not None and isinstance(message, BatchTokenIDOutput) and any(message.finished_reasons):
            span, fields = self.processing
            fields["send_cpu_ns"] = time.thread_time_ns() - span.cpu
        return value

    def install(self) -> None:
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        bindings = {
            "sglang.srt.managers.scheduler_components.request_receiver.SchedulerRequestReceiver.recv_requests": self.receive_turn,
            "sglang.srt.managers.scheduler_components.request_receiver.SchedulerRequestReceiver._broadcast_reqs_across_ranks": self.broadcast_requests,
            "sglang.srt.hardware_backend.npu.allocator_npu.NPUPagedTokenToKVPoolAllocator.alloc_extend": self.allocate_extend,
            "zmq.Socket.send_pyobj": self.send,
            "sglang.srt.managers.schedule_batch.ScheduleBatch.prepare_for_extend": self.prepare,
            "sglang.srt.managers.schedule_batch.ScheduleBatch.prepare_for_decode": self.prepare,
            "sglang.srt.managers.scheduler.Scheduler.run_batch": self.run_batch,
            "sglang.srt.managers.tp_worker.TpModelWorker.forward_batch_generation": self.forward,
            "sglang.srt.hardware_backend.npu.attention.ascend_backend.AscendAttnBackend.init_forward_metadata": self.input_metadata,
            "sglang.srt.managers.scheduler.Scheduler.process_batch_result": self.process_result,
        }
        if self.record_scheduler:
            bindings.update(
                {
                    "sglang.srt.managers.scheduler.Scheduler.get_next_batch_to_run": self.select_batch,
                    "sglang.srt.mem_cache.hiradix_cache.HiRadixCache.check_prefetch_progress": self.prefetch_progress,
                }
            )
        for target, observer in bindings.items():
            HookRegistry.register(target, observer, HookType.AROUND)
