"""Exercise the opt-in SGLang timer without importing Torch or a device runtime."""

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch


class LayerTimingTests(unittest.TestCase):
    def load(self, directory, enabled=True):
        path = Path(__file__).resolve().parents[2] / "third_party/sglang/python/sglang/srt/utils/step_timing.py"
        spec = importlib.util.spec_from_file_location("layer_timer_test", path)
        module = importlib.util.module_from_spec(spec)
        with patch.dict(
            "os.environ", {"SGLANG_STEP_TIMING_DIR": directory, "SGLANG_LAYER_WAIT_TIMING": "1" if enabled else "0"}
        ):
            spec.loader.exec_module(module)
        self.addCleanup(lambda: module._FILE.close() if module._FILE else None)
        return module

    def test_disabled_is_original(self):
        module = self.load("", False)

        def original(*args):
            return 1

        self.assertIs(module.layer_wait_timing(original), original)
        self.assertIs(module.layer_batch_timing(original), original)
        module = self.load("", True)
        self.assertIs(module.layer_wait_timing(original), original)

    def test_buffering_identity_repeats_and_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            module = self.load(directory)
            counter = SimpleNamespace(consumer_index=0)
            worker = SimpleNamespace(hicache_layer_transfer_counter=counter, tp_rank=1)
            batch = SimpleNamespace(reqs=[SimpleNamespace(rid="request-a")], forward_mode="EXTEND")

            @module.layer_wait_timing
            def wait(counter, layer):
                if layer == 2:
                    raise ValueError("test failure")
                return layer

            @module.layer_batch_timing
            def forward(worker, batch, fail=False):
                self.assertEqual(wait(counter, 0), 0)
                wait(counter, 0)
                counter.consumer_index = -1
                wait(counter, 1)
                self.assertFalse(list(Path(directory).glob("*.jsonl")))
                if fail:
                    wait(counter, 2)

            with self.assertRaisesRegex(ValueError, "test failure"):
                forward(worker, batch, fail=True)
            self.assertIsNone(module._LAYER_BATCH.get())
            (output,) = Path(directory).glob("*.jsonl")
            row = json.loads(output.read_text())
            self.assertFalse(row["returned"])
            self.assertEqual(row["identity"]["request_ids"], ["request-a"])
            self.assertEqual(row["identity"]["tp_rank"], 1)
            calls = row["identity"]["calls"]
            self.assertEqual([c["layer"] for c in calls], [0, 0, 1, 2])
            self.assertEqual([c["active"] for c in calls], [True, True, False, False])
            self.assertEqual([c["returned"] for c in calls], [True, True, True, False])
            self.assertTrue(all(c["end_ns"] >= c["start_ns"] and c["thread_cpu_ns"] >= 0 for c in calls))
            wait(counter, 1)  # Outside a forward must not emit a stray sample.
            self.assertEqual(len(output.read_text().splitlines()), 1)
            module._FILE.close()

    def test_success_and_unrelated_counter(self):
        with tempfile.TemporaryDirectory() as directory:
            module = self.load(directory)
            counter = SimpleNamespace(consumer_index=0)
            other = SimpleNamespace(consumer_index=0)
            worker = SimpleNamespace(hicache_layer_transfer_counter=counter, tp_rank=0)
            batch = SimpleNamespace(reqs=[SimpleNamespace(rid="b")], forward_mode="DECODE")
            wait = module.layer_wait_timing(lambda counter, layer: 7)

            @module.layer_batch_timing
            def forward(worker, batch):
                wait(other, 0)
                return wait(counter, 1)

            self.assertEqual(forward(worker, batch), 7)
            (output,) = Path(directory).glob("*.jsonl")
            row = json.loads(output.read_text())
            self.assertTrue(row["returned"])
            self.assertEqual(len(row["identity"]["calls"]), 1)
            self.assertEqual(row["identity"]["forward_mode"], "DECODE")
            self.assertIsNone(module._LAYER_BATCH.get())
            module._FILE.close()


if __name__ == "__main__":
    unittest.main()
