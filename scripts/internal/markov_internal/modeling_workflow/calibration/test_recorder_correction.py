"""Small conservation and overlap tests for measured recorder corrections."""

import unittest
from copy import deepcopy

from .recorder_correction import clean_reference, source_writes


class RecorderCorrectionTests(unittest.TestCase):
    def records(self):
        return [
            dict(pid=1, tid=2, name="outer", start_ns=0, end_ns=10000, thread_cpu_ns=8000),
            dict(
                pid=1,
                tid=2,
                name="next",
                start_ns=12000,
                end_ns=20000,
                thread_cpu_ns=5000,
                previous_emission=dict(start_ns=10000, end_ns=12000, thread_cpu_ns=1000),
            ),
        ]

    def test_light_nested_envelopes_each_remove_same_measured_write(self):
        records = self.records()
        records.append(dict(pid=1, tid=2, name="enclosing", start_ns=0, end_ns=21000, thread_cpu_ns=18000))
        original = deepcopy(records)
        result = clean_reference(records)
        self.assertEqual(result[0]["thread_cpu_ns"], 8000)
        self.assertEqual(result[1]["thread_cpu_ns"], 5000)
        self.assertEqual(result[2]["thread_cpu_ns"], 17000)
        self.assertEqual(records, original)

    def test_cross_boundary_is_not_prorated(self):
        records = self.records()
        records.append(dict(pid=1, tid=2, name="partial", start_ns=11000, end_ns=21000, thread_cpu_ns=8000))
        with self.assertRaisesRegex(ValueError, "crosses"):
            clean_reference(records)

        self.assertEqual(
            source_writes(self.records(), 0, 21000),
            [dict(pid=1, tid=2, method="outer", method_begin_ns=10000, method_end_ns=12000, thread_cpu_ns=1000)],
        )
