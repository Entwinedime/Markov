"""Exercise the actual storage method with fake pages, no inference runtime."""

import ast
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]


class StorageStageTimingTests(unittest.TestCase):
    def run_batch(self, enabled=True, cancel=False, fail=False, missing=False):
        with tempfile.TemporaryDirectory() as directory:
            path = ROOT / "third_party/sglang/python/sglang/srt/utils/step_timing.py"
            spec = importlib.util.spec_from_file_location("storage_stage_test", path)
            timer = importlib.util.module_from_spec(spec)
            with patch.dict(
                "os.environ",
                {
                    "SGLANG_STEP_TIMING_DIR": directory,
                    "SGLANG_HICACHE_IO_TIMING": "1" if enabled else "0",
                    "SGLANG_STEP_SCHEDSTAT": "0",
                    "SGLANG_STEP_TIMING_EMISSION": "0",
                },
            ):
                spec.loader.exec_module(timer)
            source = ast.parse((ROOT / "third_party/sglang/python/sglang/srt/managers/cache_controller.py").read_text())
            method = next(
                n for n in ast.walk(source) if isinstance(n, ast.FunctionDef) and n.name == "_generic_page_get"
            )
            method.decorator_list = []
            namespace = dict(
                storage_read_stage_recorder=timer.storage_read_stage_recorder,
                logger=SimpleNamespace(warning=lambda *args: None),
            )
            exec(compile(ast.Module(body=[method], type_ignores=[]), str(path), "exec"), namespace)
            original = namespace["_generic_page_get"]
            wrapped = timer.storage_read_timing(original)
            if not enabled:
                self.assertIs(wrapped, original)
            copied = []
            operation = SimpleNamespace(request_id="fixed", completed_tokens=0)

            def increment(tokens):
                if cancel:
                    return False
                operation.completed_tokens += tokens
                return True

            def batch_get(keys, destinations):
                if fail:
                    raise ValueError("storage failure")
                return None if missing else [object() for _ in keys]

            operation.increment = increment
            owner = SimpleNamespace(
                page_size=2,
                mem_pool_host=SimpleNamespace(
                    get_dummy_flat_data_page=lambda: object(),
                    set_from_flat_data_page=lambda index, page: copied.append(index),
                ),
                storage_backend=SimpleNamespace(batch_get=batch_get),
            )
            try:
                if fail:
                    with self.assertRaisesRegex(ValueError, "storage failure"):
                        wrapped(owner, operation, ["a", "b"], [0, 1, 2, 3])
                else:
                    self.assertIsNone(wrapped(owner, operation, ["a", "b"], [0, 1, 2, 3]))
                self.assertIsNone(timer.storage_read_stage_recorder())
                if timer._FILE:
                    timer._FILE.flush()
                files = list(Path(directory).glob("*.jsonl"))
                if not enabled:
                    self.assertEqual(files, [])
                    return copied, operation.completed_tokens, None
                rows = [json.loads(line) for line in files[0].read_text().splitlines()]
                self.assertEqual(len(rows), 1)
                row = rows[0]
                events = row["identity"]["stage_boundaries"]
                self.assertEqual(events[0]["stage"], "begin")
                self.assertEqual(events[-1]["stage"], "end")
                for field in ("time_ns", "thread_cpu_ns"):
                    self.assertEqual([e[field] for e in events], sorted(e[field] for e in events))
                self.assertLessEqual(row["start_ns"], events[0]["time_ns"])
                self.assertLessEqual(events[-1]["time_ns"], row["end_ns"])
                self.assertEqual(row["returned"], not fail)
                return copied, operation.completed_tokens, events
            finally:
                if timer._FILE:
                    timer._FILE.close()

    def test_complete(self):
        copied, tokens, events = self.run_batch()
        self.assertEqual((copied, tokens), ([0, 2], 4))
        self.assertEqual(
            [e["stage"] for e in events], ["begin", "allocated", "read", "copy", "publish", "copy", "publish", "end"]
        )
        self.assertEqual([e["page"] for e in events if e["stage"] == "copy"], [0, 1])

    def test_cancel_after_copy_does_not_publish_or_copy_next(self):
        copied, tokens, events = self.run_batch(cancel=True)
        self.assertEqual((copied, tokens), ([0], 0))
        self.assertIs(events[-2]["published"], False)

    def test_missing_and_failure(self):
        for options in ({"missing": True}, {"fail": True}):
            copied, tokens, events = self.run_batch(**options)
            self.assertEqual((copied, tokens), ([], 0))
            self.assertNotIn("copy", [e["stage"] for e in events])

    def test_disabled(self):
        self.assertEqual(self.run_batch(enabled=False), ([0, 2], 4, None))


if __name__ == "__main__":
    unittest.main()
