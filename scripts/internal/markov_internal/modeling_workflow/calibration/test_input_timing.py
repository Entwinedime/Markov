"""Input service excludes measured children once and requires complete rank coverage."""

import copy
import unittest

from .input_timing import bind_input_calls, observe_input_cpu, pair_input_cpu
from .cpu_scopes import synchronization_scopes


class InputTimingTests(unittest.TestCase):
    def inputs(self):
        parent = dict(
            name="scheduler.process_input_requests",
            pid=1,
            tid=1,
            start_ns=0,
            end_ns=10000,
            thread_cpu_ns=6000,
            returned=True,
            identity={"request_ids": ["r"], "tp_rank": 3},
        )
        child = dict(name="child", pid=1, tid=1, start_ns=2000, end_ns=4000, thread_cpu_ns=1000, returned=True)
        nested = dict(child, name="nested", start_ns=2500, end_ns=3000, thread_cpu_ns=500)
        sync = dict(
            ph="X", name="AscendCL@aclrtSynchronizeStream", pid=1, tid=1, ts=5, dur=1, args={"thread_cpu_ns": 1000}
        )
        report = dict(
            requests=[dict(kind="request", measure=True, logical_request_id="r")],
            formal_window=dict(formal_begin_ms=0, formal_end_ms=0.02),
        )
        return report, [parent, child, nested], [sync]

    def test_declared_rank_and_unique_request_coverage(self):
        report, timers, _ = self.inputs()
        self.assertEqual(bind_input_calls(report, timers, [3]), [timers[0]])
        with self.assertRaises(ValueError):
            bind_input_calls(report, timers, [0, 3])
        with self.assertRaises(ValueError):
            bind_input_calls(report, timers + [timers[0]], [3])

    def test_nested_subtraction_and_signed_pairing(self):
        _, timers, syncs = self.inputs()
        light = observe_input_cpu([timers[0]], timers, synchronization_scopes(syncs))
        self.assertEqual(light[0]["exclusive_service_cpu_us"], 4.0)
        self.assertEqual(light[0]["exclusive_ranges_ns"], [[0, 2000], [4000, 10000]])
        full_timers = copy.deepcopy(timers)
        full_timers[0]["thread_cpu_ns"] = 5000
        full = observe_input_cpu([full_timers[0]], full_timers, synchronization_scopes(syncs))
        self.assertEqual(pair_input_cpu(light, full)[0]["measured_service_delta_us"], -1.0)
        full[0]["child_calls"] = 2
        with self.assertRaises(ValueError):
            pair_input_cpu(light, full)

    def test_crossing_sync_is_not_partially_subtracted(self):
        _, timers, syncs = self.inputs()
        syncs[0].update(ts=3, dur=3)
        with self.assertRaises(ValueError):
            observe_input_cpu([timers[0]], timers, synchronization_scopes(syncs))

    def test_pair_order_is_independent_of_trace_file_order(self):
        _, timers, syncs = self.inputs()
        rows = observe_input_cpu([timers[0]], timers, synchronization_scopes(syncs))
        other = copy.deepcopy(rows[0])
        other["identity"]["request_ids"] = ["another_request"]
        rows.append(other)
        self.assertEqual(pair_input_cpu(rows, rows), pair_input_cpu(rows[::-1], rows[::-1]))
