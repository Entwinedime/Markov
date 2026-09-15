"""Device-free checks: observations must not alter response delivery."""

import asyncio
import types
import unittest
from unittest.mock import Mock, patch

from trace_sim_probe.probes import response_boundaries as probe


def install(index, name, owner):
    module = types.ModuleType(probe.TARGET_MODULES[index])
    setattr(module, name, owner)
    probe.install(module)
    return module


class ResponseBoundaryCheck(unittest.IsolatedAsyncioTestCase):
    def test_sender_records_only_real_terminal_sends(self):
        class SenderWrapper:
            socket = object()

            def send_output(self, output, fail=False):
                if fail:
                    raise ValueError("send failed")
                return output

        module = install(0, "SenderWrapper", SenderWrapper)
        wrapped = SenderWrapper.send_output
        probe.install(module)
        self.assertIs(SenderWrapper.send_output, wrapped)
        sender = SenderWrapper()
        terminal = types.SimpleNamespace(rids=["done", "pending"], finished_reasons=[{}, None])
        writer = Mock()
        with patch.object(probe, "get_writer", return_value=writer):
            self.assertIs(sender.send_output(terminal), terminal)
            sender.send_output(types.SimpleNamespace())
            with self.assertRaisesRegex(ValueError, "send failed"):
                sender.send_output(terminal, fail=True)
            sender.socket = None
            sender.send_output(terminal)
        writer.duration_event.assert_called_once()
        self.assertEqual(writer.duration_event.call_args.args[0], "runtime.response.scheduler_send")
        self.assertEqual(writer.duration_event.call_args.args[4], {"request_ids": ["done"]})

    async def test_tokenizer_preserves_async_return_and_failure(self):
        class TokenizerManager:
            async def _handle_batch_output(self, output, fail=False):
                await asyncio.sleep(0)
                if fail:
                    raise ValueError("dispatch failed")
                return output

        install(1, "TokenizerManager", TokenizerManager)
        output = types.SimpleNamespace(rids=["request"], finished_reasons=[{}])
        writer = Mock()
        with patch.object(probe, "get_writer", return_value=writer):
            self.assertIs(await TokenizerManager()._handle_batch_output(output), output)
            with self.assertRaisesRegex(ValueError, "dispatch failed"):
                await TokenizerManager()._handle_batch_output(output, fail=True)
        writer.duration_event.assert_called_once()

    async def test_response_forwards_original_messages_and_marks_final_body(self):
        messages = [
            {"type": "http.response.start", "status": 200},
            {"type": "http.response.body", "body": b"first", "more_body": True},
            {"type": "http.response.body", "body": b"last"},
        ]

        class Response:
            def __init__(self, content=None, status_code=200):
                self.content = content
                self.status_code = status_code

            async def __call__(self, scope, receive, send):
                for message in messages:
                    await send(message)
                return "original result"

        class SGLangORJSONResponse(Response):
            pass

        original = Response.__call__
        module = install(2, "SGLangORJSONResponse", SGLangORJSONResponse)
        wrapped = SGLangORJSONResponse.__call__
        probe.install(module)
        self.assertIs(SGLangORJSONResponse.__call__, wrapped)
        self.assertIs(Response.__call__, original)
        content = [{"meta_info": {"id": "done", "finish_reason": {}}}, {"meta_info": None}]
        writer = Mock()
        forwarded = []

        async def send(message):
            forwarded.append(message)

        with patch.object(probe, "get_writer", return_value=writer):
            response = SGLangORJSONResponse(content, status_code=201)
            self.assertIs(response.content, content)
            self.assertEqual(response.status_code, 201)
            self.assertEqual(await response({}, None, send), "original result")
        self.assertEqual(len(forwarded), len(messages))
        self.assertTrue(all(left is right for left, right in zip(forwarded, messages)))
        self.assertEqual([call.args[0] for call in writer.duration_event.call_args_list],
                         ["runtime.response.serialize", "runtime.response.http_body_sent"])

        async def failed_send(message):
            raise ValueError("network failed")

        writer.reset_mock()
        with patch.object(probe, "get_writer", return_value=writer):
            with self.assertRaisesRegex(ValueError, "network failed"):
                await response({}, None, failed_send)
        writer.duration_event.assert_not_called()

    def test_partial_module_is_not_marked_installed(self):
        for name in probe.TARGET_MODULES:
            probe.install(types.ModuleType(name))

    def test_receiver_observes_only_nonempty_results_without_changing_them(self):
        class SchedulerRequestReceiver:
            def _pull_raw_reqs(self):
                if self.fail:
                    raise ValueError("receive failed")
                return self.requests

            def recv_requests(self):
                return self._pull_raw_reqs()

        module = install(3, "SchedulerRequestReceiver", SchedulerRequestReceiver)
        wrapped = SchedulerRequestReceiver.recv_requests
        probe.install(module)
        self.assertIs(wrapped, SchedulerRequestReceiver.recv_requests)
        receiver = SchedulerRequestReceiver()
        receiver.fail = False
        receiver.requests = [types.SimpleNamespace(rid="one"), types.SimpleNamespace(batch=[types.SimpleNamespace(rid="two")])]
        writer = Mock()
        with patch.object(probe, "get_writer", return_value=writer):
            self.assertIs(receiver.recv_requests(), receiver.requests)
        self.assertEqual([call.args[0] for call in writer.duration_event.call_args_list],
                         ["runtime.request.socket_received", "runtime.request.dispatch_ready"])
        for call in writer.duration_event.call_args_list:
            self.assertEqual(call.args[1], call.args[2])
            self.assertEqual(call.args[4], {"request_ids": ["one", "two"]})
        with patch.object(probe, "get_writer") as get_writer:
            for empty in (None, [], [types.SimpleNamespace(control="flush")]):
                receiver.requests = empty
                self.assertIs(receiver.recv_requests(), empty)
            receiver.fail = True
            with self.assertRaisesRegex(ValueError, "receive failed"):
                receiver.recv_requests()
            get_writer.assert_not_called()


if __name__ == "__main__":
    unittest.main()
