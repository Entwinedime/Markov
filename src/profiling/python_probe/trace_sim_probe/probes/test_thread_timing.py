"""Optional thread diagnostics must release files even after invalid reads."""

import io
import unittest
from unittest.mock import patch

from trace_sim_probe.probes.thread_timing import _thread_schedstat_snapshot


class ThreadTimingTests(unittest.TestCase):
    def test_read_closes_file_and_preserves_missing_measurement(self) -> None:
        for text, expected in [
            ("10 20 3", {"schedstat_runtime_ns": 10, "schedstat_runqueue_delay_ns": 20, "schedstat_timeslices": 3}),
            ("10 20", None),
            ("invalid 20 3", None),
            ("-1 20 3", None),
        ]:
            with self.subTest(text=text):
                stream = io.StringIO(text)
                with patch("builtins.open", return_value=stream):
                    self.assertEqual(_thread_schedstat_snapshot(), expected)
                self.assertTrue(stream.closed)

    def test_unavailable_proc_is_not_zero(self) -> None:
        with patch("builtins.open", side_effect=PermissionError):
            self.assertIsNone(_thread_schedstat_snapshot())
