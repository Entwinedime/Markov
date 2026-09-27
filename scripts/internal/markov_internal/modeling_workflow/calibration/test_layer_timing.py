"""Layer CPU budgets use paired base work, not target timing."""

import copy
import unittest
from .layer_timing import observe_layer_cpu, pair_layer_cpu


class LayerCpuTests(unittest.TestCase):
    def fixture(self):
        step = dict(
            request_id="a",
            identity={"tp_rank": 0},
            name="step[EXTEND bs=1]",
            pid=1,
            tid=2,
            start_ns=1000,
            end_ns=10000,
            thread_cpu_ns=8000,
        )
        call = dict(layer=0, active=True, returned=True, start_ns=2000, end_ns=4000, thread_cpu_ns=1000)
        batch = dict(
            name="hicache.layer_wait_batch",
            pid=1,
            tid=2,
            start_ns=0,
            end_ns=11000,
            returned=True,
            identity=dict(request_ids=["a"], tp_rank=0, calls=[call]),
        )
        return [step], [batch]

    def test_signed_reserved_budget(self):
        steps, events = self.fixture()
        light = observe_layer_cpu(steps, events, [])
        events[0]["identity"]["calls"][0]["thread_cpu_ns"] = 100
        full = observe_layer_cpu(steps, events, [])
        comparison = {"rows": [dict(request_id="a", rank=0, phase="step[EXTEND", delta={"outside_sync_cpu_us": 8.0})]}
        pair = pair_layer_cpu(light, full, comparison, "source")
        self.assertEqual(comparison["rows"][0]["layer_reduction_us"], -1)
        self.assertEqual(comparison["rows"][0]["delta"]["outside_sync_cpu_us"], 8.0)
        self.assertEqual(pair["rows"][0]["measured_service_delta_us"], -1)
        self.assertEqual(pair["rows"][0]["exclusive_ranges_ns"], [[2000, 4000]])

    def test_invalid_calls_and_sync(self):
        for field, value in [("returned", False), ("start_ns", 0), ("end_ns", 12000), ("thread_cpu_ns", -1)]:
            steps, events = self.fixture()
            events[0]["identity"]["calls"][0][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                observe_layer_cpu(steps, events, [])
        steps, events = self.fixture()
        with self.assertRaises(ValueError):
            observe_layer_cpu(steps, events, [dict(pid=1, tid=2, start_ns=2000, end_ns=3000)])
        with self.assertRaises(ValueError):
            observe_layer_cpu(steps, events + events, [])

    def test_repeated_layer_preserved_and_work_mismatch_rejected(self):
        steps, events = self.fixture()
        call = copy.deepcopy(events[0]["identity"]["calls"][0])
        call.update(start_ns=5000, end_ns=7000)
        events[0]["identity"]["calls"].append(call)
        rows = observe_layer_cpu(steps, events, [])
        self.assertEqual(len(next(iter(rows.values()))["calls"]), 2)
        changed = copy.deepcopy(rows)
        next(iter(changed.values()))["calls"][1]["active"] = False
        comparison = {"rows": [dict(request_id="a", rank=0, phase="step[EXTEND", delta={"outside_sync_cpu_us": 8})]}
        with self.assertRaises(ValueError):
            pair_layer_cpu(rows, changed, comparison, "source")

    def test_repeated_forward_keeps_separate_step_budgets(self):
        steps, events = self.fixture()
        later, batch = copy.deepcopy(steps[0]), copy.deepcopy(events[0])
        for item in (later, batch, batch["identity"]["calls"][0]):
            item["start_ns"] += 20000
            item["end_ns"] += 20000
        later["forward_ordinal"] = 1
        rows = observe_layer_cpu([later, *steps], [batch, *events], [])
        self.assertEqual([r["cpu_us"] for r in rows.values()], [1, 1])
        comparison = {
            "rows": [
                dict(request_id="a", rank=0, phase="step[EXTEND", forward_ordinal=i, delta={"outside_sync_cpu_us": 8})
                for i in range(2)
            ]
        }
        paired = pair_layer_cpu(rows, rows, comparison, "source")
        self.assertEqual([r["exclusive_ranges_ns"] for r in paired["rows"]], [[[2000, 4000]], [[22000, 24000]]])
        self.assertEqual(comparison["rows"][0]["layer_reduction_us"], 0)
