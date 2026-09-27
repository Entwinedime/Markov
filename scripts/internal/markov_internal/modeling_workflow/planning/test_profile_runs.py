"""Missing manifest labels must not merge independent source runs."""

import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from ...common.paths import require_repo_path
from .profile_runs import parse_profile_run, parse_server_command_tokens
from .specs import plan_model_runs
from ..preflight import preflight_sources
from ..types import TargetHiCacheConfig


class ProfileIdentityTests(unittest.TestCase):
    def parse(self, directory, config=None, **fields):
        root = require_repo_path(directory)
        manifest = {"run_dir": str(root), "config_path": str(root / "config.json"), **fields}
        if config is None:
            config = {"metadata": {"suite_server_id": "base", "suite_input_id": "workload"}}
        with (
            patch("markov_internal.modeling_workflow.planning.profile_runs.load_json", side_effect=[manifest, config]),
            patch(
                "markov_internal.modeling_workflow.planning.profile_runs.extract_hicache_modeling_config",
                return_value={"page_size": 128},
            ),
        ):
            return parse_profile_run(root / "profile_manifest.json")

    def test_ordinary_profile_labels_do_not_require_matrix_metadata(self):
        expected = {"model_path": "model", "tp_size": "2", "numa_node": "4 4"}
        for model, tp in (("--model-path", "--tp-size"), ("--model", "--tensor-parallel-size"), ("--model", "--tp")):
            self.assertEqual(
                parse_server_command_tokens(["python", "server.py", model, "model", tp, "2", "--numa-node", "4", "4"]),
                expected,
            )
        self.assertEqual(parse_server_command_tokens(["--tp-size=2", "--tensor-parallel-size", "4"]), {"tp_size": "4"})

        for config, expected in (
            ({"name": "chat"}, ("chat", "chat")),
            ({}, ("data/first/run", "data/first/run")),
            ({"name": "chat", "metadata": {"config_id": "base", "workload_id": "short"}}, ("base", "short")),
            (
                {"metadata": {"config_id": "ignored", "suite_server_id": "matrix", "suite_input_id": "input"}},
                ("matrix", "input"),
            ),
        ):
            with self.subTest(config=config):
                source = self.parse("data/first/run", config=config)
                self.assertEqual((source.config_id, source.input_id), expected)

    def test_preflight_and_planning_share_source_blockers(self):
        source = self.parse("data/first/run")
        options = SimpleNamespace(
            output_dir=Path("unused"),
            diagnostics=SimpleNamespace(keep_debug_artifacts=False),
            trace_threads=1,
            trace_file_threads=1,
            cpu_service_costs={},
            hicache_io_model=None,
        )
        module = "markov_internal.modeling_workflow.preflight."
        with patch(module + "write_json") as write, patch(module + "audit_hicache_profile") as audit:
            for dag_ready, hicache_ready in ((True, True), (True, False), (False, True), (False, False)):
                audit.return_value = {
                    "trace_channel_coverage": {
                        "torch_trace_files": int(dag_ready),
                        "ld_preload_trace_files": 1,
                        "python_probe_trace_files": 1,
                    },
                    "artifact_errors": [] if dag_ready else ["trace_channel_missing"],
                    "artifact_ready": dag_ready,
                    "missing_trace_channels": [] if dag_ready else ["torch"],
                    "configured_target_count": 1,
                    "observed_target_count": 1,
                    "requested_consumers": ["hicache_state_model"],
                    "workflow_input_ready": hicache_ready,
                    "workflow_input_errors": [] if hicache_ready else ["hicache_token_dictionary_missing"],
                }
                report = preflight_sources(options, [source])
                ready = dag_ready and hicache_ready
                self.assertEqual(report["ready"], ready)
                self.assertEqual(report["full_trace_ready_count"], int(dag_ready))
                self.assertEqual(report["workflow_input_ready_count"], int(hicache_ready))
                self.assertEqual(write.call_args.args[1], report)
                row = report["sources"][0]
                self.assertEqual(row["manifest_path"], str(source.manifest_path))
                self.assertEqual(row["workflow_input_errors"], audit.return_value["workflow_input_errors"])
                self.assertIsNone(row["audit_path"])
                target = TargetHiCacheConfig("target", {"page_size": 128})
                options.target_configs = (target,)
                for model in (None, object()):
                    options.hicache_io_model = model
                    (spec,) = plan_model_runs(options, [source], report)
                    if not dag_ready:
                        expected = "full_dag_trace_not_ready:trace_channel_missing"
                    elif model is None:
                        expected = "missing_hicache_io_model"
                    elif not hicache_ready:
                        expected = "source_workflow_input_not_ready"
                    else:
                        expected = ""
                    self.assertEqual(spec.skip_reason, expected)
