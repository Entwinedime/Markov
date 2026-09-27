"""Scheduler scopes retain their relationship to the measured forward call."""

import copy
import unittest

from .scheduler_timing import observe_scheduler_cpu, pair_scheduler_cpu
from .cpu_scopes import validate_host_ranges


def scheduler_work():
    identity = dict(request_ids=["r"], tp_rank=0)

    def scope(name, start, end, cpu, **extra):
        return dict(
            name=name,
            pid=1,
            tid=1,
            start_ns=start,
            end_ns=end,
            thread_cpu_ns=cpu,
            returned=True,
            identity=dict(identity, **extra),
        )

    forward = scope("step[EXTEND bs=1 toks=128]", 4000, 6000, 1000)
    forward["request_id"] = "r"
    steps = dict(source_manifest="base", rows=[forward])
    hosts = dict(source_manifest="base", rows=[], unbound=[], partial=[])
    report = dict(formal_window=dict(formal_begin_ms=0, formal_end_ms=0.02))
    timers = [
        scope("scheduler.get_next_batch_to_run", 1000, 2000, 500, selected_request_ids=["r"]),
        scope("scheduler.run_batch", 3000, 7000, 2000, forward_mode="1"),
        scope("scheduler.process_batch_result", 8000, 9000, 500, forward_mode="1"),
    ]
    return steps, hosts, report, timers


class SchedulerTimingTests(unittest.TestCase):
    def test_three_methods_bind_same_forward_without_double_counting(self):
        result = observe_scheduler_cpu(*scheduler_work(), [])
        self.assertEqual([r["forward_start_ns"] for r in result["rows"]], [4000] * 3)
        self.assertEqual([r["exclusive_service_cpu_us"] for r in result["rows"]], [0.5, 1.0, 0.5])
        self.assertEqual(result["rows"][1]["exclusive_ranges_ns"], [[3000, 4000], [6000, 7000]])

    def test_empty_selection_and_window_tail_remain_visible(self):
        steps, hosts, report, timers = scheduler_work()
        timers[0]["identity"]["selected_request_ids"] = []
        partial = copy.deepcopy(timers[2])
        partial.update(start_ns=19000, end_ns=21000)
        outside = dict(partial, start_ns=21000, end_ns=22000)
        result = observe_scheduler_cpu(steps, hosts, report, timers + [partial, outside], [])
        self.assertEqual(result["rows"][0]["phase"], "empty")
        self.assertEqual(result["partial"], [partial])
        self.assertEqual(result["outside_http"], [outside])

    def test_duplicate_binding_and_wrong_phase_are_rejected(self):
        steps, hosts, report, timers = scheduler_work()
        with self.assertRaises(ValueError):
            observe_scheduler_cpu(steps, hosts, report, timers + [timers[0]], [])
        timers[2]["identity"]["forward_mode"] = "2"
        with self.assertRaises(ValueError):
            observe_scheduler_cpu(steps, hosts, report, timers, [])

    def test_boundary_cost_uses_children_outside_scoring_window(self):
        steps, hosts, report, timers = scheduler_work()
        timers[-1].update(start_ns=19000, end_ns=22000, thread_cpu_ns=2000)
        child = dict(
            name="hicache.host.test", pid=1, tid=1, start_ns=20500, end_ns=21500, thread_cpu_ns=500, returned=True
        )
        timers.append(child)
        result = observe_scheduler_cpu(steps, hosts, report, timers, [], measure_boundary_results=True)
        boundary = result["boundary_observations"][0]
        self.assertEqual(boundary["child_calls"], 1)
        self.assertEqual(boundary["exclusive_service_cpu_us"], 1.5)
        self.assertEqual(boundary["exclusive_ranges_ns"], [[19000, 20500], [21500, 22000]])
        self.assertEqual(len(result["rows"]), 2)
        child["end_ns"] = 23000
        with self.assertRaises(ValueError):
            observe_scheduler_cpu(steps, hosts, report, timers, [], measure_boundary_results=True)

    def test_unrelated_partial_host_does_not_reject_complete_scheduler_work(self):
        steps, hosts, report, timers = scheduler_work()
        hosts["partial"] = [dict(pid=1, tid=1, start_ns=19000, end_ns=21000)]
        self.assertEqual(len(observe_scheduler_cpu(steps, hosts, report, timers, [])["rows"]), 3)
        hosts["partial"][0]["start_ns"] = 8500
        with self.assertRaisesRegex(ValueError, "crosses an in-window"):
            observe_scheduler_cpu(steps, hosts, report, timers, [])


class SchedulerPairingTests(unittest.TestCase):
    def test_overlap_results_follow_fifo_and_pair_each_decode_separately(self):
        steps, hosts, report, timers = scheduler_work()
        report["formal_window"]["formal_end_ms"] = 0.05
        steps["rows"][0]["name"] = "step[DECODE bs=1]"
        for row in timers[1:]:
            row["identity"]["forward_mode"] = "2"
        second = copy.deepcopy(steps["rows"][0])
        second.update(start_ns=14000, end_ns=16000, forward_ordinal=1)
        steps["rows"].append(second)
        later = copy.deepcopy(timers)
        for row in later:
            row["start_ns"] += 10000
            row["end_ns"] += 10000
        # Both results are processed after both forwards have been launched.
        timers[2].update(start_ns=18000, end_ns=19000)
        later[2].update(start_ns=20000, end_ns=21000)
        light = observe_scheduler_cpu(steps, hosts, report, list(reversed(timers + later)), [])
        results = [row for row in light["rows"] if row["name"] == "scheduler.process_batch_result"]
        self.assertEqual([row["forward_start_ns"] for row in results], [4000, 14000])
        self.assertEqual([row["forward_ordinal"] for row in results], [0, 1])
        full = copy.deepcopy(light)
        full["source_manifest"] = "profiled"
        for row in full["rows"]:
            row["exclusive_service_cpu_us"] += row["forward_ordinal"] + 0.25
        paired = pair_scheduler_cpu(light, full)
        self.assertEqual(len(paired["rows"]), 6)
        for row in paired["rows"]:
            self.assertEqual(row["measured_service_delta_us"], row["forward_ordinal"] + 0.25)

    def observations(self):
        light = observe_scheduler_cpu(*scheduler_work(), [])
        full = copy.deepcopy(light)
        full["source_manifest"] = "profiled"
        return light, full

    def test_signed_cost_and_empty_selection(self):
        light, full = self.observations()
        full["rows"][0]["exclusive_service_cpu_us"] -= 0.25
        result = pair_scheduler_cpu(light, full)
        self.assertEqual(result["rows"][0]["measured_service_delta_us"], -0.25)
        light["rows"][0]["phase"] = full["rows"][0]["phase"] = "empty"
        self.assertEqual(len(pair_scheduler_cpu(light, full)["rows"]), 2)

    def test_missing_tail_requires_boundary_evidence(self):
        light, full = self.observations()
        tail = full["rows"].pop()
        with self.assertRaises(ValueError):
            pair_scheduler_cpu(light, full)
        full["outside_http"].append(tail)
        self.assertEqual(len(pair_scheduler_cpu(light, full)["unmatched_light_keys"]), 1)
        full["rows"][0]["child_calls"] += 1
        with self.assertRaises(ValueError):
            pair_scheduler_cpu(light, full)

    def test_complete_light_boundary_supplies_cost_not_source_geometry(self):
        steps, hosts, report, timers = scheduler_work()
        profiled = observe_scheduler_cpu(steps, hosts, report, timers, [])
        timers[-1].update(start_ns=19000, end_ns=21000, thread_cpu_ns=1000)
        light = observe_scheduler_cpu(steps, hosts, report, timers, [], measure_boundary_results=True)
        self.assertEqual(len(light["rows"]), 2)
        self.assertEqual(len(light["boundary_observations"]), 1)
        paired = pair_scheduler_cpu(light, profiled)
        result = next(row for row in paired["rows"] if row["method"] == "scheduler.process_batch_result")
        self.assertEqual(result["normal_service_cpu_us"], 1.0)
        self.assertEqual(result["method_begin_ns"], 8000)
        self.assertEqual(result["exclusive_ranges_ns"], [[8000, 9000]])
        self.assertEqual(len(paired["light_boundary_keys"]), 1)
        timers[-1]["returned"] = False
        incomplete = observe_scheduler_cpu(steps, hosts, report, timers, [], measure_boundary_results=True)
        with self.assertRaises(ValueError):
            pair_scheduler_cpu(incomplete, profiled)

    def test_paired_ranges_reject_duplicate_ownership(self):
        light, full = self.observations()
        paired = pair_scheduler_cpu(light, full)
        validate_host_ranges(paired["rows"])
        with self.assertRaises(ValueError):
            validate_host_ranges(paired["rows"] + paired["rows"])

    def test_light_run_wrapper_may_cross_http_end_without_changing_forward_work(self):
        steps, hosts, report, timers = scheduler_work()
        profiled = observe_scheduler_cpu(steps, hosts, report, timers, [])
        # The forward is still fully in-window, but the lightweight wrapper ends later.
        timers[1].update(end_ns=21000, thread_cpu_ns=2500)
        timers[-1].update(start_ns=22000, end_ns=23000)
        timers.append(steps["rows"][0])
        light = observe_scheduler_cpu(steps, hosts, report, timers, [], measure_boundary_results=True)
        paired = pair_scheduler_cpu(light, profiled)
        run = next(row for row in paired["rows"] if row["method"] == "scheduler.run_batch")
        self.assertEqual(run["normal_service_cpu_us"], 1.5)
        self.assertEqual(run["method_end_ns"], 7000)
        self.assertEqual(run["exclusive_ranges_ns"], [[3000, 4000], [6000, 7000]])

    def test_profiled_run_wrapper_at_boundary_is_retained_without_correction(self):
        light, full = self.observations()
        run = full["rows"].pop(1)
        full["partial"].append(run)
        paired = pair_scheduler_cpu(light, full)
        self.assertEqual(len(paired["rows"]), 2)
        self.assertEqual(paired["unmatched_light_keys"][0][2], "scheduler.run_batch")
