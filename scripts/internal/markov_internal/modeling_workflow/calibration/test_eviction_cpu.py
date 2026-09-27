"""Locked-loop sampling calls the current framework body, not a copied approximation."""

import ast
import heapq
import math
from pathlib import Path
from tempfile import TemporaryDirectory
import time
from types import SimpleNamespace as NS
import unittest
from unittest.mock import patch

from ...common.io import load_json
from ...common.paths import ROOT_DIR
from . import eviction_cpu as module
from .. import physical_capture
from ..group import PhysicalBudget


class EvictionCpuTests(unittest.TestCase):
    def test_coefficients_use_endpoints_not_interior_checks(self):
        rows = []
        for rank in (0, 1):
            for size in (1, 8, 64):
                cost = 0.3 * size + 0.04 * math.lgamma(size + 1) / math.log(2)
                rows.append(dict(rank=rank, heap_size=size, pairs=[dict(baseline_us=2.0, active_us=2.0 + cost)]))
        report = dict(clock="thread_time_ns", samples=rows, limitations=[])
        coefficients, evidence = module.estimate_locked_candidate_cost(report)
        self.assertAlmostEqual(coefficients["locked_candidate_us"], 0.3)
        self.assertAlmostEqual(coefficients["locked_candidate_log2_heap_us"], 0.04)
        self.assertEqual(len(evidence["checks"]), 6)
        rows[1]["pairs"][0]["active_us"] += 10
        unchanged, check = module.estimate_locked_candidate_cost(report)
        self.assertEqual(unchanged, coefficients)
        self.assertLess(check["checks"][1]["relative_error"], -0.5)
        rows[0]["pairs"][0]["active_us"] = 1.0
        with self.assertRaises(ValueError):
            module.estimate_locked_candidate_cost(report)

    def test_group_budget_failure_and_reuse_share_one_ledger(self):
        with TemporaryDirectory(dir=ROOT_DIR) as directory:
            group = NS(
                output_dir=Path(directory),
                sources=[NS(run_dir=Path(directory))],
                raw={"physical_capture": {"cpu_sets": "0", "eviction_cpu": {"heap_sizes": [1, 8], "repeats": 2}}},
                physical_budget=PhysicalBudget(wall_seconds=120, container_starts=0, logical_io_bytes=0),
            )
            needs = [{"component": "execution_control/eviction_locked_candidate", "coordinates": [{"heap_size": 1}]}]
            count = 0

            def run(row, command, remaining, **kwargs):
                nonlocal count
                count += 1
                row.update(status="failed" if count == 1 else "completed", wall_seconds=2.0)
                if count > 1:
                    module.write_json(
                        ROOT_DIR / row["report"],
                        dict(
                            status="measured",
                            operation="eviction_locked_candidate",
                            target_workload_trace_used=False,
                            target_score_used=False,
                            samples=[dict(rank=0, heap_size=size, cpu_affinity=[0], pairs=[{}, {}]) for size in (1, 8)],
                        ),
                    )

            with (
                patch(
                    "markov_internal.modeling_workflow.group.source_environment",
                    return_value={"server": {"tp_size": "1"}},
                ),
                patch(
                    "markov_internal.modeling_workflow.planning.profile_runs.parse_server_command_flags",
                    return_value={},
                ),
                patch.object(physical_capture, "run_container_attempt", side_effect=run) as launch,
                patch("markov_internal.modeling_workflow.capture.time.monotonic", side_effect=[100, 103, 200, 204]),
            ):
                self.assertEqual(module.acquire_eviction_cpu(group, needs)["status"], "physical_budget_exhausted")
                launch.assert_not_called()
                group.physical_budget = PhysicalBudget(wall_seconds=120, container_starts=1, logical_io_bytes=0)
                self.assertEqual(module.acquire_eviction_cpu(group, needs, dry_run=True)["stop_reason"], "dry_run")
                launch.assert_not_called()
                self.assertEqual(module.acquire_eviction_cpu(group, needs)["status"], "failed")
                self.assertEqual(module.acquire_eviction_cpu(group, needs)["status"], "physical_budget_exhausted")
                group.physical_budget = PhysicalBudget(wall_seconds=120, container_starts=2, logical_io_bytes=0)
                self.assertEqual(module.acquire_eviction_cpu(group, needs)["status"], "cpu_primitive_measured")
                reused = module.acquire_eviction_cpu(group, needs)
                self.assertTrue(reused["reused"])
                self.assertEqual(reused["usage"], dict(wall_seconds=7.0, container_starts=2, logical_io_bytes=0))
                self.assertEqual(launch.call_count, 2)

    def test_current_framework_loop_skips_exactly_the_locked_candidates(self):
        # Compile only the real method to test host-side without importing NPU libraries.
        source = ROOT_DIR / "third_party/sglang/python/sglang/srt/mem_cache/hiradix_cache.py"
        tree = ast.parse(source.read_text())
        cls = next(node for node in tree.body if isinstance(node, ast.ClassDef) and node.name == "HiRadixCache")
        method = next(node for node in cls.body if isinstance(node, ast.FunctionDef) and node.name == "evict")
        namespace = dict(time=time, heapq=heapq, EvictParams=NS, EvictResult=NS)
        exec(compile(ast.Module(body=[method], type_ignores=[]), str(source), "exec"), namespace)
        nodes = [NS(lock_ref=1, last_access_time=float(index)) for index in range(8)]
        cache = NS(
            evictable_leaves=nodes,
            eviction_strategy=NS(get_priority=lambda node: node.last_access_time),
            cache_controller=NS(write_policy="write_through"),
            update_eviction_metrics=lambda *_: None,
        )
        with patch.object(heapq, "heappop", wraps=heapq.heappop) as pop:
            rows = module.measure_locked_candidates(namespace["evict"], cache, NS, batch=3, repeats=2)
        self.assertEqual(pop.call_count, 8 * 3 * (1 + 2))
        self.assertEqual([row["order"] for row in rows], [[0, 1], [1, 0]])
        self.assertEqual(len(cache.evictable_leaves), 8)
        self.assertTrue(all(node.lock_ref == 1 for node in nodes))

    def test_raw_negative_measurements_and_partial_failure_survive(self):
        with TemporaryDirectory() as directory:
            output = Path(directory) / "eviction_cpu.json"
            with (
                patch.object(module.os, "sched_getaffinity", return_value={0, 1}),
                patch.object(module.os, "sched_setaffinity") as affinity,
                patch.object(
                    module,
                    "measure_locked_candidates",
                    side_effect=[[{"loop_us": -0.1}], RuntimeError("measurement interrupted")],
                ),
            ):
                with self.assertRaisesRegex(RuntimeError, "measurement interrupted"):
                    module.capture_locked_candidates(
                        None, NS, None, None, heap_sizes=[1, 8], cpu_sets=[{0}], batch=2, repeats=1, output=output
                    )
            report = load_json(output)
            self.assertEqual(report["status"], "failed")
            self.assertEqual(report["samples"][0]["median_loop_us"], -0.1)
            self.assertFalse(report["target_workload_trace_used"])
            affinity.assert_called_with(0, {0, 1})
