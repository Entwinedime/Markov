"""CPU counter endpoints for cross-call waiting, without a device runtime."""

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


class WaitTimingTests(unittest.TestCase):
    def load(self, directory, enabled=True):
        path = Path(__file__).resolve().parents[2] / "third_party/sglang/python/sglang/srt/utils/step_timing.py"
        spec = importlib.util.spec_from_file_location("wait_timer_test", path)
        module = importlib.util.module_from_spec(spec)
        with patch.dict(
            "os.environ",
            {
                "SGLANG_STEP_TIMING_DIR": directory,
                "SGLANG_PREFETCH_TIMING": "1" if enabled else "0",
                "SGLANG_STEP_TIMING_EMISSION": "1",
                "SGLANG_STEP_SCHEDSTAT": "0",
            },
        ):
            spec.loader.exec_module(module)
        self.addCleanup(lambda: module._FILE.close() if module._FILE else None)
        return module

    def test_cross_call_cpu_and_emission_endpoints(self):
        with tempfile.TemporaryDirectory() as directory:
            module = self.load(directory)
            with patch.object(
                module.time, "thread_time_ns", side_effect=[100, 140, 150, 170, 250, 290, 300, 320]
            ) as clock:
                with module.step_timing("hicache.wait.progress", {"request_ids": ["a"], "ready": False}):
                    pass
                with module.step_timing("hicache.wait.progress", {"request_ids": ["a"], "ready": True}):
                    pass
            rows = [json.loads(line) for line in next(Path(directory).glob("*.jsonl")).read_text().splitlines()]
            self.assertEqual(clock.call_count, 8)  # Same two scope/two emission reads as before.
            for row in rows:
                self.assertEqual(row["thread_cpu_ns"], row["thread_cpu_end_ns"] - row["thread_cpu_start_ns"])
            previous = rows[1]["previous_emission"]
            self.assertEqual(previous["thread_cpu_ns"], 20)
            self.assertEqual(previous["thread_cpu_end_ns"] - previous["thread_cpu_start_ns"], 20)
            # Inter-call CPU includes emission, unlike either individual call.
            self.assertEqual(rows[1]["thread_cpu_start_ns"] - rows[0]["thread_cpu_end_ns"], 110)
            self.assertLessEqual(previous["thread_cpu_end_ns"], rows[1]["thread_cpu_start_ns"])

    def test_disabled_preserves_original_wrapper_and_schema(self):
        with tempfile.TemporaryDirectory() as directory:
            module = self.load(directory, False)

            def original():
                return 7

            self.assertIs(module.prefetch_timing("hicache.wait.progress")(original), original)
            with module.step_timing("forward"):
                pass
            with module.step_timing("forward"):
                pass
            rows = [json.loads(line) for line in next(Path(directory).glob("*.jsonl")).read_text().splitlines()]
            self.assertNotIn("thread_cpu_start_ns", rows[0])
            self.assertNotIn("thread_cpu_start_ns", rows[1]["previous_emission"])

    def test_failure_keeps_endpoints_and_does_not_swallow_error(self):
        with tempfile.TemporaryDirectory() as directory:
            module = self.load(directory)
            with self.assertRaisesRegex(ValueError, "test failure"):
                with module.step_timing("hicache.wait.receive"):
                    raise ValueError("test failure")
            row = json.loads(next(Path(directory).glob("*.jsonl")).read_text())
            self.assertFalse(row["returned"])
            self.assertEqual(row["thread_cpu_ns"], row["thread_cpu_end_ns"] - row["thread_cpu_start_ns"])


if __name__ == "__main__":
    unittest.main()
