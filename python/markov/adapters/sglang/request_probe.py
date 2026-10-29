"""Observe request transport and the batch results consumed by the scheduler."""

import time

from markov.instrumentation.events import EventWriter


class RequestProbe:
    def __init__(self, writer: EventWriter):
        from sglang.srt.managers.io_struct import (
            BatchTokenIDOutput,
            BatchTokenizedGenerateReqInput,
            TokenizedGenerateReqInput,
        )

        self.writer = writer
        self.input_type = TokenizedGenerateReqInput
        self.batch_input_type = BatchTokenizedGenerateReqInput
        self.output_type = BatchTokenIDOutput
        self.batch_sequence = 0
        self.running_batch = None
        self.processing_batch = None
        self.results = {}

    def run_batch(self, original, scheduler, batch, *args, **kwargs):
        self.batch_sequence += 1
        identity = self.batch_sequence
        phase = "other"
        if batch.forward_mode.is_decode():
            phase = "decode"
        elif batch.forward_mode.is_extend():
            phase = "prefill"
        fields = {
            "batch_id": identity,
            "request_ids": [request.rid for request in batch.reqs],
            "phase": phase,
            "overlap": scheduler.enable_overlap,
        }
        self.running_batch = identity
        try:
            with self.writer.span("runtime.request.batch_run", fields):
                result = original(scheduler, batch, *args, **kwargs)
            # The scheduler owns queued results; the probe retains no tensors.
            self.results[id(result)] = identity
            return result
        finally:
            self.running_batch = None

    def copy_result(self, original, result, return_logprob, return_hidden_states=True):
        tokens = result.next_token_ids
        fields = {
            "batch_id": self.running_batch if self.running_batch is not None else self.results.get(id(result)),
            "return_logprob": return_logprob,
            "return_hidden_states": return_hidden_states and result.logits_output.hidden_states is not None,
            "additional_outputs": any(
                value is not None
                for value in (
                    result.accept_lens,
                    result.routed_experts_output,
                    result.indexer_topk_output,
                    result.expert_distribution_metrics,
                )
            ),
            "token_shape": list(tokens.shape),
            "token_dtype": str(tokens.dtype),
            "token_device": tokens.device.type,
            "token_bytes": tokens.numel() * tokens.element_size(),
        }
        with self.writer.span("runtime.response.copy_to_cpu", fields):
            return original(result, return_logprob, return_hidden_states)

    def process_result(self, original, scheduler, batch, result):
        identity = self.results.pop(id(result), None)
        requests = list(batch.reqs)
        fields = {
            "batch_id": identity,
            "request_ids": [request.rid for request in requests],
            "output_tokens_before": [len(request.output_ids) for request in requests],
            "finished_before": [request.finished() for request in requests],
        }
        self.processing_batch = identity
        try:
            with self.writer.span("runtime.response.process_result", fields):
                value = original(scheduler, batch, result)
                fields.update(
                    output_tokens_after=[len(request.output_ids) for request in requests],
                    finished_after=[request.finished() for request in requests],
                )
                return value
        finally:
            self.processing_batch = None

    def receive(self, original, socket, *args, **kwargs):
        message = original(socket, *args, **kwargs)
        if isinstance(message, self.input_type):
            identities = [message.rid]
        elif isinstance(message, self.batch_input_type):
            identities = [request.rid for request in message.batch]
        else:
            return message

        received = time.time_ns()
        self.writer.write("runtime.request.socket_received", received, received, {"request_ids": identities})
        return message

    def send(self, original, socket, message, *args, **kwargs):
        if not isinstance(message, self.output_type):
            return original(socket, message, *args, **kwargs)

        # Intermediate streaming messages are not request completion ports.
        identities = [rid for rid, reason in zip(message.rids, message.finished_reasons) if reason is not None]
        if not identities:
            return original(socket, message, *args, **kwargs)

        fields = {"request_ids": identities, "batch_id": self.processing_batch}
        with self.writer.span("runtime.response.scheduler_send", fields):
            return original(socket, message, *args, **kwargs)

    def install(self):
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        HookRegistry.register("zmq.Socket.recv_pyobj", self.receive, HookType.AROUND)
        HookRegistry.register("zmq.Socket.send_pyobj", self.send, HookType.AROUND)
        HookRegistry.register("sglang.srt.managers.scheduler.Scheduler.run_batch", self.run_batch, HookType.AROUND)
        HookRegistry.register(
            "sglang.srt.managers.scheduler.Scheduler.process_batch_result", self.process_result, HookType.AROUND
        )
        HookRegistry.register(
            "sglang.srt.managers.utils.GenerationBatchResult.copy_to_cpu", self.copy_result, HookType.AROUND
        )
