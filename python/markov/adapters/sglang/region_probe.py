"""Measure eager semantic regions, without native hooks or profiler exports."""

import time

from markov.instrumentation.events import EventWriter


IMPLEMENTATION = "sglang.ascend.eager"


class RegionProbe:
    def __init__(self, writer: EventWriter):
        self.writer = writer
        self.running = None
        self.forward_event = None
        self.preparations = {}
        self.seen_inputs = set()
        self.seen_phases = set()
        self.results = {}
        self.pending = []
        self.processing = None

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
        import torch

        fields = {"implementation": IMPLEMENTATION}
        start = torch.npu.Event(enable_timing=True)
        end = torch.npu.Event(enable_timing=True)
        begin = time.time_ns()
        start.record()
        with self.writer.span("region.prepare", fields):
            result = original(batch)
            fields.update(self.variables(batch))
            phase, features = fields["phase"], fields["features"]
            identity = (phase, features["query_tokens"], features["batch_size"], features["page_tokens"])
            features.update(
                first_phase=int(phase not in self.seen_phases),
                new_input_size=int(phase in self.seen_phases and identity not in self.seen_inputs),
            )
            self.seen_phases.add(phase)
            self.seen_inputs.add(identity)
        end.record()
        self.pending.append(("region.prepare_stream", start, end, begin, fields))
        self.preparations[id(batch)] = fields
        return result

    def run_batch(self, original, scheduler, batch, *args, **kwargs):
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
            return result
        finally:
            self.running = None
            self.forward_event = None

    def forward(self, original, worker, *args, **kwargs):
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
        return original(worker, *args, **kwargs)

    def process_result(self, original, scheduler, batch, result):
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
                self.writer.write(
                    name,
                    begin,
                    begin,
                    {**measured, "stream_interval_ns": round(start.elapsed_time(end) * 1_000_000)},
                )
            else:
                pending.append((name, start, end, begin, measured))
        self.pending = pending
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
            "zmq.Socket.send_pyobj": self.send,
            "sglang.srt.managers.schedule_batch.ScheduleBatch.prepare_for_extend": self.prepare,
            "sglang.srt.managers.schedule_batch.ScheduleBatch.prepare_for_decode": self.prepare,
            "sglang.srt.managers.scheduler.Scheduler.run_batch": self.run_batch,
            "sglang.srt.managers.tp_worker.TpModelWorker.forward_batch_generation": self.forward,
            "sglang.srt.managers.scheduler.Scheduler.process_batch_result": self.process_result,
        }
        for target, observer in bindings.items():
            HookRegistry.register(target, observer, HookType.AROUND)
