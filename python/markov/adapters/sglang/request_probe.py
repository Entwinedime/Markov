"""Observe scheduler request ports at the actual message transport."""

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

        with self.writer.span("runtime.response.scheduler_send", {"request_ids": identities}):
            return original(socket, message, *args, **kwargs)

    def install(self):
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        HookRegistry.register("zmq.Socket.recv_pyobj", self.receive, HookType.AROUND)
        HookRegistry.register("zmq.Socket.send_pyobj", self.send, HookType.AROUND)
