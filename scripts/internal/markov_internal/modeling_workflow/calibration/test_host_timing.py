"""Immediate-child CPU and synchronization are each subtracted exactly once."""

import copy
import unittest

from .host_timing import observe_host_cpu, pair_host_cpu
from .cpu_scopes import synchronization_scopes


class HostTimingTests(unittest.TestCase):
    def inputs(self):
        steps = dict(source_manifest="base", rows=[])
        report = {"formal_window": dict(formal_begin_ms=0.0, formal_end_ms=1.0, e2e_ms=1.0)}

        def timer(name, begin, end, cpu):
            return dict(name=name, pid=1, tid=1, start_ns=begin, end_ns=end, thread_cpu_ns=cpu, returned=True)

        outer = timer("scheduler.run_batch", 0, 1000000, 700000)
        outer["identity"] = dict(request_ids=["r"], tp_rank=0)
        records = [
            outer,
            timer("hicache.host.parent", 100000, 900000, 600000),
            timer("hicache.host.child", 200000, 400000, 100000),
            timer("hicache.host.grandchild", 250000, 300000, 30000),
        ]
        syncs = [
            dict(
                ph="X",
                name="AscendCL@aclrtSynchronizeStream",
                pid=1,
                tid=1,
                ts=at,
                dur=20 if cpu == 10000 else 60,
                args={"thread_cpu_ns": cpu},
            )
            for at, cpu in ((260, 10000), (500, 40000))
        ]
        return steps, report, records, syncs

    def test_nested_children_and_sync_are_not_double_counted(self):
        steps, report, records, syncs = self.inputs()
        result = observe_host_cpu(steps, report, records, synchronization_scopes(syncs))
        self.assertEqual([r["exclusive_service_cpu_us"] for r in result["rows"]], [460.0, 70.0, 20.0])
        self.assertEqual([r["child_calls"] for r in result["rows"]], [1, 1, 0])
        self.assertEqual(
            [r["exclusive_ranges_ns"] for r in result["rows"]],
            [[[100000, 200000], [400000, 900000]], [[200000, 250000], [300000, 400000]], [[250000, 300000]]],
        )
        self.assertEqual(result["unbound"], [])

    def test_partial_cpu_is_not_prorated_and_identity_is_required(self):
        steps, report, records, syncs = self.inputs()
        report["formal_window"]["formal_begin_ms"] = 0.15
        result = observe_host_cpu(steps, report, records, synchronization_scopes(syncs))
        self.assertEqual(len(result["partial"]), 1)
        self.assertEqual(result["partial"][0]["thread_cpu_ns"], 600000)
        self.assertEqual(result["partial"][0]["identity"]["request_ids"], ["r"])
        result = observe_host_cpu(steps, report, records[1:], synchronization_scopes(syncs))
        self.assertEqual(len(result["unbound"]), 2)

    def test_sync_cannot_cross_child_boundary(self):
        for crossing in (True, False):
            steps, report, records, syncs = self.inputs()
            if crossing:
                syncs[0]["ts"] = 390
            else:
                syncs.append(dict(syncs[-1]))
            with self.subTest(crossing=crossing), self.assertRaises(ValueError):
                observe_host_cpu(steps, report, records, synchronization_scopes(syncs))

    def test_formal_request_tail_is_observed_whole_outside_http_window(self):
        steps, report, records, _ = self.inputs()
        steps["rows"] = [dict(request_id="r")]
        outer = dict(records[0], start_ns=2000000, end_ns=3000000)
        host = dict(records[1], start_ns=2100000, end_ns=2900000)
        result = observe_host_cpu(steps, report, [outer, host], [])
        self.assertEqual(result["rows"], [])
        self.assertEqual(result["outside_http"][0]["identity"]["request_ids"], ["r"])
        self.assertEqual(result["outside_http"][0]["thread_cpu_ns"], 600000)
        steps["rows"] = [dict(request_id="other")]
        with self.assertRaisesRegex(ValueError, "No measured HiCache"):
            observe_host_cpu(steps, report, [outer, host], [])


class HostPairingTests(unittest.TestCase):
    def inputs(self):
        row = dict(
            name="hicache.host.start_loading",
            parent_host=None,
            identity={"request_ids": ["r"], "tp_rank": 2},
            pid=50,
            tid=50,
            start_ns=100000,
            end_ns=120000,
            child_calls=0,
            exclusive_ranges_ns=[[100000, 120000]],
            exclusive_service_cpu_us=10.0,
        )
        light = {"source_manifest": "light", "rows": [row]}
        full = copy.deepcopy(light)
        full["source_manifest"] = "profiled"
        full["rows"][0]["exclusive_service_cpu_us"] = 14.0
        facts = [
            dict(
                name="hicache_loadback_io_observed_end",
                pid="50",
                tid="50",
                ts=99,
                dur=22,
                args={"status": "completed", "operation_id": 7},
            )
        ]
        return light, full, facts

    def test_signed_delta_bound_to_source_call(self):
        light, full, facts = self.inputs()
        for value in (14.0, 6.0):
            full["rows"][0]["exclusive_service_cpu_us"] = value
            row = pair_host_cpu(light, full, facts)["rows"][0]
            self.assertEqual(row["measured_service_delta_us"], value - 10.0)
            self.assertEqual(row["exclusive_ranges_ns"], [[100000, 120000]])
            self.assertEqual((row["rank"], row["operation_id"]), (2, 7))

    def test_missing_duplicate_or_incomplete_probe_rejected(self):
        for case in ("missing", "duplicate", "incomplete", "thread"):
            light, full, facts = self.inputs()
            if case == "missing":
                facts.clear()
            elif case == "duplicate":
                facts.append(copy.deepcopy(facts[0]))
            elif case == "incomplete":
                facts[0]["args"]["status"] = "started"
            else:
                facts[0]["tid"] = "other"
            with self.subTest(case=case), self.assertRaises(ValueError):
                pair_host_cpu(light, full, facts)

    def test_changed_identity_or_nesting_rejected(self):
        for case in ("identity", "nesting", "partial"):
            light, full, facts = self.inputs()
            if case == "identity":
                light["rows"][0]["identity"]["request_ids"] = ["other"]
            elif case == "nesting":
                light["rows"][0]["child_calls"] = 1
            else:
                full["partial"] = [full["rows"][0]]
            with self.subTest(case=case), self.assertRaises(ValueError):
                pair_host_cpu(light, full, facts)

    def test_http_boundary_cost_is_retained_not_prorated(self):
        light, full, facts = self.inputs()
        full["partial"] = [full["rows"].pop()]
        result = pair_host_cpu(light, full, facts)
        self.assertEqual(result["rows"], [])
        self.assertEqual(len(result["uncorrected_http_boundary_keys"]), 1)

    def test_unidentified_boundary_is_not_silently_skipped(self):
        light, full, facts = self.inputs()
        full["partial"] = [full["rows"].pop()]
        del full["partial"][0]["identity"]
        with self.assertRaisesRegex(ValueError, "explicit request identity"):
            pair_host_cpu(light, full, facts)

    def test_complete_light_tail_supplies_cost_but_not_source_geometry(self):
        light, full, facts = self.inputs()
        light["outside_http"] = [light["rows"].pop()]
        light["outside_http"][0].update(start_ns=200000, end_ns=220000)
        result = pair_host_cpu(light, full, facts)
        row = result["rows"][0]
        self.assertEqual(row["normal_service_cpu_us"], 10.0)
        self.assertEqual(row["measured_service_delta_us"], 4.0)
        self.assertEqual(row["exclusive_ranges_ns"], [[100000, 120000]])
        self.assertEqual(len(result["light_boundary_keys"]), 1)
