"""Device-free checks for batch-buffered HiCache wait observations."""

import types
import unittest
from unittest.mock import Mock, patch

from trace_sim_probe.probes import layer_waits as probe
from trace_sim_probe.writer import _jsonable


class LayerWaitCheck(unittest.TestCase):
    def setUp(self):
        class LayerDoneCounter:
            consumer_index = -1
            num_layers = 64

            def wait_until(self, threshold):
                if threshold < 0:
                    raise ValueError("invalid layer")
                return threshold

        class TpModelWorker:
            def forward_batch_generation(self, batch, fail=False):
                counter = getattr(self, "hicache_layer_transfer_counter", None)
                if batch is None or counter is None:
                    return "skipped"
                counter.consumer_index = batch.hicache_consumer_index
                for layer in range(counter.num_layers):
                    counter.wait_until(layer)
                    counter.wait_until(layer)
                if fail:
                    counter.wait_until(-1)
                return "result"

        for name, owner in zip(probe.TARGET_MODULES, (LayerDoneCounter, TpModelWorker)):
            module = types.ModuleType(name)
            setattr(module, owner.__name__, owner)
            probe.install(module)
            method = "wait_until" if owner is LayerDoneCounter else "forward_batch_generation"
            wrapped = getattr(owner, method)
            probe.install(module)
            self.assertIs(wrapped, getattr(owner, method))
        self.worker = TpModelWorker()
        self.counter = LayerDoneCounter()
        self.worker.hicache_layer_transfer_counter = self.counter
        self.batch = types.SimpleNamespace(reqs=[types.SimpleNamespace(rid="request")],
                                           forward_mode=types.SimpleNamespace(name="DECODE"),
                                           hicache_consumer_index=0)
        self.writer = Mock()
        self.writer.now_us.side_effect = range(100, 200)
        writer_patch = patch.object(probe, "get_writer", return_value=self.writer)
        writer_patch.start()
        self.addCleanup(writer_patch.stop)

    def test_waits_are_serialized_once_per_batch_without_truncation(self):
        self.assertEqual(self.worker.forward_batch_generation(self.batch), "result")
        self.writer.duration_event.assert_called_once()
        event = self.writer.duration_event.call_args.args
        self.assertEqual(event[:4], ("runtime.hicache.layer_waits", 100, 101, "runtime_diagnostic"))
        record = _jsonable(event[4])
        self.assertEqual(record["request_ids"], ["request"])
        self.assertEqual(record["status"], "returned")
        self.assertEqual(record["phase"], "DECODE")
        self.assertEqual(record["layer_count"], 64)
        self.assertEqual([row[0] for row in record["wait_intervals"]],
                         [layer for layer in range(64) for _ in range(2)])
        self.assertTrue(all(end >= start for _, start, end in record["wait_intervals"]))
        self.assertIsNone(probe._BATCH.get())

    def test_no_consumer_records_an_empty_batch(self):
        self.batch.hicache_consumer_index = -1
        self.worker.forward_batch_generation(self.batch)
        self.assertEqual(self.writer.duration_event.call_args.args[4]["wait_intervals"], [])

    def test_exception_propagates_and_clears_batch_context(self):
        with self.assertRaisesRegex(ValueError, "invalid layer"):
            self.worker.forward_batch_generation(self.batch, fail=True)
        self.assertEqual(self.writer.duration_event.call_args.args[4]["status"], "raised")
        self.assertIsNone(probe._BATCH.get())

    def test_outside_batch_and_other_counter_are_not_observed(self):
        self.counter.consumer_index = 0
        self.assertEqual(self.counter.wait_until(2), 2)
        intervals = []
        token = probe._BATCH.set((object(), intervals))
        try:
            self.assertEqual(self.counter.wait_until(3), 3)
        finally:
            probe._BATCH.reset(token)
        self.assertEqual(intervals, [])
        self.writer.duration_event.assert_not_called()

    def test_absent_batch_or_hicache_preserves_original_call(self):
        self.assertEqual(self.worker.forward_batch_generation(None), "skipped")
        del self.worker.hicache_layer_transfer_counter
        self.assertEqual(self.worker.forward_batch_generation(self.batch), "skipped")
        self.writer.duration_event.assert_not_called()
