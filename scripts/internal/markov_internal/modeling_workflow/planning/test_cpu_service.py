"""One source's measured CPU service is shared across its target predictions."""

from dataclasses import replace
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from ...common.paths import require_repo_path, repo_relative_path
from ..context import DiagnosticLevel
from ..context import WorkflowOptions, cpu_service_inputs
from ..execution.runner_adapter import runner_config
from ..types import ProfileRunRef, TargetHiCacheConfig
from .specs import plan_model_runs


class CpuServicePlanningTests(unittest.TestCase):
    def test_explicit_source_identity(self):
        source = require_repo_path("base/profile_manifest.json").resolve()
        with patch(
            "markov_internal.modeling_workflow.context.load_json",
            return_value={"source_manifest": "base/profile_manifest.json"},
        ):
            inputs = cpu_service_inputs([Path("data/service.json")], (source,))
            self.assertEqual(inputs, {source: require_repo_path("data/service.json")})
            with self.assertRaises(ValueError):
                cpu_service_inputs([Path("data/service.json")], ())
            with self.assertRaises(ValueError):
                cpu_service_inputs([Path("data/service.json"), Path("data/another.json")], (source,))

    def test_targets_share_only_their_source_input(self):
        root = require_repo_path("data/test_source")
        source = ProfileRunRef(
            root / "profile_manifest.json",
            root,
            root / "config.json",
            "base",
            "base",
            "workload",
            (),
            {"prefetch_policy": "timeout"},
        )
        # Display names need not be unique across independently captured profiles.
        other = replace(source, config_id="other", manifest_path=root / "other.json")
        service = root / "service.json"
        targets = tuple(
            TargetHiCacheConfig(name, {"page_size": 64, "prefetch_policy": "timeout"}) for name in ("A", "B")
        )
        options = WorkflowOptions(
            (source.manifest_path, other.manifest_path),
            targets,
            root,
            DiagnosticLevel("off"),
            cpu_service_costs={source.manifest_path: service},
        )
        runs = [source, other]
        report = {
            "sources": [
                {"manifest_path": str(source.manifest_path), "full_trace_ready": True, "skip_reason": ""},
                {
                    "manifest_path": str(other.manifest_path),
                    "full_trace_ready": False,
                    "skip_reason": "full_dag_trace_not_ready",
                },
            ]
        }
        specs = plan_model_runs(options, runs, report)
        self.assertEqual(len(specs), len(runs) * len(targets))
        window = SimpleNamespace(
            start_ns=1000, end_ns=9000, actual_e2e_ns=8000, source="test", report_path=root / "report.json"
        )
        with patch("markov_internal.modeling_workflow.types.discover_workload_window", return_value=window) as discover:
            for spec in specs:
                payload = runner_config(spec, options).to_raw()
                self.assertEqual(payload["cpp_trace_graph"]["backend_kind"], "release")
                self.assertFalse(payload["outputs"]["emit_module_summary"])
                self.assertNotIn("actual_e2e_us", payload["cpp_trace_graph"])
                self.assertEqual(payload["cpp_trace_graph"]["trace_window_end_us"], 9)
                debug = runner_config(spec, replace(options, diagnostics=DiagnosticLevel.FULL)).to_raw()
                self.assertEqual(debug["cpp_trace_graph"]["backend_kind"], "validation")
                self.assertTrue(debug["outputs"]["emit_module_summary"])
                self.assertEqual(debug["cpp_trace_graph"]["actual_e2e_us"], 8)
                if spec.source.config_id == "base":
                    self.assertEqual(spec.skip_reason, "missing_hicache_io_model")
                    self.assertEqual(payload["input"]["cpu_service_cost"], str(repo_relative_path(service)))
                else:
                    self.assertEqual(spec.skip_reason, "full_dag_trace_not_ready")
                    self.assertNotIn("cpu_service_cost", payload["input"])
            self.assertEqual(discover.call_count, 2)
