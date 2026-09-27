"""Execution evidence must not masquerade as a complete static patch/cost ledger."""

from types import SimpleNamespace
from dataclasses import replace
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch

from ...common.io import load_json, write_json
from ..types import ModelRunResult, ModelRunSpec, TargetHiCacheConfig
from .. import workflow
from ..context import DiagnosticLevel, WorkflowOptions
from .hicache import build_row, read_row, summarize


class ExecutionEvidenceTests(unittest.TestCase):
    def fixture(self):
        target = TargetHiCacheConfig("target", {"page_size": 64, "prefetch_policy": "wait_complete"})
        spec = ModelRunSpec(
            run_id="run",
            output_dir=Path("unused"),
            source=SimpleNamespace(
                run_id="base",
                config_id="source",
                input_id="workload",
                manifest_path=Path("source.json"),
                hicache_config=None,
            ),
            target=target,
        )
        result = ModelRunResult(spec, 0)
        execution = {
            "status": "executed",
            "cost_coverage": "partial",
            "prepared_facts": 12,
            "consumed_facts": 12,
            "http_us": 42,
            "remaining_approximations": ["source_residual_waits"],
            "phase_admissions": 2,
            "confirmations": {"partial_window_rounds": 0},
        }
        return result, execution

    def test_incomplete_execution_does_not_pass(self):
        result, execution = self.fixture()
        run = {"module_results": {"hicache_execution": execution}}
        execution["consumed_facts"] -= 1
        self.assertEqual(build_row(result, run)["status"], "NOT_READY")

        execution["consumed_facts"] += 1
        execution["confirmations"]["partial_window_rounds"] = 1
        self.assertEqual(build_row(result, run)["status"], "NOT_READY")

    def test_formal_prediction_does_not_reconstruct_static_patch_details(self):
        result, _ = self.fixture()
        row = build_row(
            result, {"module_results": {"hicache_dag_patch": {"status": "applied", "topology_valid": True}}}
        )
        self.assertEqual(row["status"], "NOT_READY")
        self.assertEqual(row["blockers"], ["execution_result_missing"])

    def test_completed_is_not_cost_ready(self):
        result, execution = self.fixture()
        run = {"module_results": {"hicache_execution": execution}}
        with TemporaryDirectory() as directory:
            result = replace(result, spec=replace(result.spec, output_dir=Path(directory)))
            report = result.spec.output_dir / "run_summary.json"
            write_json(report, run)
            pending = summarize([read_row(replace(result, skip_reason="not_started"))])
            self.assertEqual(pending["completed_count"], 0)
            self.assertEqual(pending["cells"][0]["blockers"], ["not_started"])
            self.assertIsNone(pending["cells"][0]["http_e2e_us"])

            summary = summarize([read_row(result)])
            self.assertEqual(load_json(report), run)
            evidence = summary["cells"][0]
        self.assertNotIn("execution", evidence)
        self.assertNotIn("structure_ready", evidence)
        self.assertEqual(evidence["target_hicache"], result.spec.target.fields)
        self.assertEqual(evidence["source_manifest"], "source.json")
        self.assertEqual(summary["status"], "EXECUTED")
        self.assertEqual(summary["completed_count"], 1)
        self.assertEqual(summary["cost_coverage_counts"], {"partial": 1})
        self.assertEqual(summary["blocker_counts"], {})
        self.assertEqual(summary["cells"][0]["http_e2e_us"], 42)
        self.assertEqual(summary["cells"][0]["approximations"], ["source_residual_waits"])
        execution.pop("cost_coverage")
        row = build_row(result, run)
        self.assertEqual(summarize([row])["cost_coverage_counts"], {"unknown": 1})
        failed = build_row(replace(result, return_code=1), run)
        summary = summarize([row, failed])
        self.assertEqual(summary["status"], "CHECK")
        self.assertEqual(summary["completed_count"], 1)
        self.assertIsNone(summary["cells"][1]["http_e2e_us"])

    def test_cost_gaps_are_shared_without_fabricating_completion(self):
        result, _ = self.fixture()
        missing_costs = (
            {
                "component": "execution_control/eviction_locked_candidate",
                "coordinates": {"heap_size": 8},
                "reason": "missing measurement",
            },
        )
        # Failed attempts must not consume a previous successful run summary.
        result = replace(result, return_code=1, missing_costs=missing_costs)
        failed_summary = summarize([read_row(result)])
        self.assertEqual(failed_summary["completed_count"], 0)
        self.assertEqual(failed_summary["cells"][0]["blockers"], ["model_command_failed"])
        row = build_row(result, {})
        rows = [row, {**row, "model_run_id": "another_target"}]
        summary = summarize(rows)
        self.assertEqual(summary["completed_count"], 0)
        self.assertFalse(summary["cost_requirements_complete"])
        self.assertEqual(
            summary["missing_costs"],
            [
                {
                    "component": "execution_control/eviction_locked_candidate",
                    "coordinates": [{"heap_size": 8}],
                    "reasons": ["missing measurement"],
                    "cells": ["run", "another_target"],
                }
            ],
        )
        self.assertTrue(all(cell["http_e2e_us"] is None for cell in summary["cells"]))

    def test_interrupted_workflow_keeps_completed_cells(self):
        result, execution = self.fixture()
        with TemporaryDirectory() as directory:
            output = Path(directory)
            result = replace(result, spec=replace(result.spec, output_dir=output / "run"))
            pending = replace(result.spec, run_id="pending", output_dir=output / "pending")
            write_json(
                result.spec.output_dir / "run_summary.json", {"module_results": {"hicache_execution": execution}}
            )
            options = WorkflowOptions((), (), output, DiagnosticLevel.OFF)

            def execute(*args):
                yield result
                raise KeyboardInterrupt()

            with (
                patch.object(workflow, "discover_profile_runs", return_value=[]),
                patch.object(workflow, "preflight_sources", return_value={"ready": True}),
                patch.object(workflow, "plan_model_runs", return_value=[result.spec, pending]),
                patch.object(workflow, "run_model_runs", side_effect=execute),
                self.assertRaises(KeyboardInterrupt),
            ):
                workflow.run_workflow(options)

            summary = load_json(output / "workflow_summary.json")["prediction"]
            self.assertEqual(summary["completed_count"], 1)
            self.assertEqual(summary["status"], "CHECK")
            self.assertEqual([row["http_e2e_us"] for row in summary["cells"]], [42, None])
