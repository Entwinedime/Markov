"""Counter transforms must not reuse the vendor singleton's first rank."""

import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from profiling.profiler_clock import read_host_clock


class ProfilerClockCheck(unittest.TestCase):
    def test_rank_local_transform_and_counter_disabled_case(self):
        class Config:
            def load_info(self, root):
                self._start_cnt = int(Path(root).name.split("_")[0])
                self._syscnt_enable = self._start_cnt != 3
                self._freq = 100

            def get_timestamp_from_syscnt(self, tick):
                return (tick - self._start_cnt) * 10 + 123 if self._syscnt_enable else tick

            def get_local_time(self, ns):
                return ns + 1_700_000_000_000_000_000

        singleton = Config()
        singleton._start_cnt = 999
        vendor = SimpleNamespace(ProfilerConfig=lambda: singleton)
        with tempfile.TemporaryDirectory() as directory, patch.dict(sys.modules, {
            "torch_npu.profiler.analysis._profiler_config": vendor,
        }):
            for rank in (1, 2, 3):
                root = Path(directory) / f"{rank}_ascend_pt"
                root.mkdir()
                (root / "profiler_info.json").write_text("{}")
                clock = read_host_clock(root / "ASCEND_PROFILER_OUTPUT" / "trace_view.json")
                self.assertEqual(clock["origin_tick"], rank if rank != 3 else 0)
                self.assertEqual(clock["ns_per_tick"], 10 if rank != 3 else 1)
                self.assertEqual(clock["origin_ns"], 1_700_000_000_000_000_000 + (123 if rank != 3 else 0))
            self.assertEqual(singleton._start_cnt, 999)
            self.assertIsNone(read_host_clock(Path(directory) / "other" / "trace_view.json"))
