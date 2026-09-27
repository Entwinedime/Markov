"""Small checks for the profiling-to-modeling workload report handoff."""

import json
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from profiling.manifest import build_profile_manifest
from profiling.config import normalize_profiling_config
from markov_internal.modeling.workload import discover_workload_window
from markov_internal.profiling.environments import apply_python_probe_env
from markov_internal.profiling.probe_targets import select_python_probe_targets
from markov_internal.common.manifest import manifest_files
from markov_internal.modeling_workflow.validations.hicache.preflight.profile import audit_hicache_profile


class WorkloadManifestCheck(unittest.TestCase):
    def test_partial_trace_loss_cannot_be_hidden_by_another_file(self):
        first, second = self.root / "first.json", self.root / "second.json"
        entries = [{"path": str(first)}, {"path": str(second)}]
        with self.assertRaisesRegex(ValueError, "unavailable"):
            manifest_files([{"path": str(second), "exists": False}])
        for channel, section, key in (
            ("torch", "trace", "torch_trace_files"),
            ("ld_preload", "trace", "ld_preload_trace_files"),
            ("python_probe", "sidecar", "python_probe_files"),
        ):
            with self.subTest(channel=channel):
                first.write_text("[]", encoding="utf-8")
                second.write_text("[]", encoding="utf-8")
                self.manifest.write_text(
                    json.dumps(
                        {
                            "profiling": {"channels_enabled": [channel]},
                            section: {key: entries},
                        }
                    ),
                    encoding="utf-8",
                )
                audit = audit_hicache_profile(self.manifest)
                self.assertEqual(audit["trace_channel_coverage"][f"{channel}_trace_files"], 2)
                second.unlink()
                self.assertEqual(manifest_files(entries), [first, second])
                with self.assertRaisesRegex((FileNotFoundError, ValueError), "second.json"):
                    audit_hicache_profile(self.manifest)

    def test_emission_diagnostic_is_not_a_model_fact(self):
        with patch.object(sys, "path", [str(Path(__file__).parent / "python_probe"), *sys.path]):
            from trace_sim_probe.probes import generic_callable as probe
        from markov_internal.modeling_workflow.validations.hicache.core.facts import parse_fact_or_none
        from unittest.mock import Mock

        target = probe._parse_target(
            {
                "id": "hicache_controller.writeback_enqueue_observed",
                "module": "test",
                "target": "call",
                "events": {"end": "hicache_enqueue_end"},
                "fields": [],
                "capture_emission_timing": True,
                "fact": {
                    "class": "source_actual",
                    "role": "writeback_enqueue_observed",
                    "consumers": ["hicache_dag_patch"],
                },
            }
        )
        writer = Mock()
        with patch.object(probe, "get_writer", return_value=writer):
            probe._emit_targets((target,), None, (), {}, None, "end", 1, 2, None)
        calls = writer.duration_event.call_args_list
        self.assertIsNotNone(parse_fact_or_none(calls[0].args[4]))
        self.assertEqual({call.args[4]["stage"] for call in calls[1:]}, {"binding", "fields", "writer"})
        for call in calls[1:]:
            self.assertIsNone(parse_fact_or_none(call.args[4]))

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.report = self.root / "bench" / "case" / "workload_report.json"
        self.report.parent.mkdir(parents=True)
        self.report.write_text(json.dumps({"formal_window": {"formal_begin_ms": 100, "formal_end_ms": 150}}))
        self.manifest = self.root / "profile_manifest.json"

    def build_manifest(self, cfg=None):
        return build_profile_manifest(
            run_dir=self.root,
            cfg=cfg or {},
            runtime=normalize_profiling_config(cfg or {}),
            started_at=0,
            ended_at=1,
            status="completed",
            dry_run=False,
        )

    def test_declared_report_is_preferred_to_directory_discovery(self):
        manifest = self.build_manifest()
        self.assertEqual(manifest["bench"]["workload_report_files"][0]["path"], str(self.report))
        self.assertTrue(manifest["bench"]["workload_report_files"][0]["exists"])
        self.manifest.write_text(json.dumps(manifest))
        extra = self.root / "bench" / "unrelated" / "workload_report.json"
        extra.parent.mkdir()
        extra.write_text("{}")
        window = discover_workload_window({}, self.manifest)
        self.assertEqual(window.report_path, self.report)
        self.assertEqual(window.actual_e2e_ns, 50_000_000)

    def test_multiple_declared_reports_require_selection(self):
        manifest = self.build_manifest()
        manifest["bench"]["workload_report_files"] *= 2
        self.manifest.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, "multiple workload reports"):
            discover_workload_window({}, self.manifest)
        self.assertEqual(
            discover_workload_window({"workload_report": str(self.report)}, self.manifest).report_path, self.report
        )

    def test_undeclared_report_is_not_discovered(self):
        self.manifest.write_text(json.dumps({"run_dir": str(self.root)}))
        self.assertIsNone(discover_workload_window({}, self.manifest))

    def test_absent_report_is_not_invented(self):
        self.report.unlink()
        manifest = self.build_manifest()
        self.assertEqual(manifest["bench"]["workload_report_files"], [])
        self.manifest.write_text(json.dumps(manifest))
        self.assertIsNone(discover_workload_window({}, self.manifest))
        # Aggregate duration has no trace-clock origin and must not become [0, duration].
        bench_report = self.root / "bench.jsonl"
        bench_report.write_text(json.dumps({"duration": 1.25}) + "\n", encoding="utf-8")
        manifest["bench"]["bench_serving_files"] = [{"path": str(bench_report)}]
        self.manifest.write_text(json.dumps(manifest))
        self.assertIsNone(discover_workload_window({}, self.manifest))
        with self.assertRaisesRegex(ValueError, "cannot define a trace window"):
            discover_workload_window({"workload_report": str(bench_report)}, self.manifest)

    def test_each_torch_trace_carries_its_own_clock(self):
        for directory in ("trace/torch", "custom/device", str(self.root / "absolute/device")):
            with self.subTest(directory=directory):
                cfg = {"profiling": {"torch": {"output_dir": directory}}}
                for rank in (1, 2):
                    trace = self.root / directory / str(rank) / "trace_view.json"
                    trace.parent.mkdir(parents=True)
                    trace.write_text("[]")

                clocks = [{"origin_tick": 10}, {"origin_tick": 20}]
                with patch("profiling.manifest.read_host_clock", side_effect=clocks) as read:
                    manifest = self.build_manifest(cfg)
                entries = manifest["trace"]["torch_trace_files"]
                self.assertEqual(manifest["trace"]["torch_trace_dir"], str(self.root / directory))
                self.assertEqual([e["host_clock"] for e in entries], clocks)
                self.assertEqual([call.args[0] for call in read.call_args_list], [Path(e["path"]) for e in entries])

    def test_dag_runtime_observations_are_independent_of_diagnostics(self):
        for diagnostics in ("off", "timing"):
            for consumer in ("hicache_state_model", "hicache_dag_patch"):
                with self.subTest(diagnostics=diagnostics, consumer=consumer):
                    cfg = {
                        "profiling": {
                            "channels": ["python_probe"],
                            "python_probe": {"consumers": [consumer], "diagnostics": diagnostics},
                        }
                    }
                    runtime = normalize_profiling_config(cfg)
                    names = runtime.to_manifest_fragment()["python_runtime_probes"]
                    causal = consumer == "hicache_dag_patch" or diagnostics != "off"
                    for name in ("response_boundaries", "cpu_collectives", "layer_waits", "decode_allocation"):
                        self.assertEqual(name in names, causal)
                    self.assertEqual("runtime_preparation" in names, causal)
                    env = {}
                    selected = select_python_probe_targets(runtime.python_consumers, diagnostics=diagnostics)
                    apply_python_probe_env(env, runtime, SimpleNamespace(trace_dir=self.root), selected)
                    self.assertEqual(env["TRACE_SIM_PYTHON_PROBE_CONSUMERS"], consumer)
                    self.assertEqual(env["TRACE_SIM_PYTHON_PROBE_DIAGNOSTICS"], diagnostics)
                    targets = json.loads(env["TRACE_SIM_PYTHON_PROBE_TARGETS"])
                    enqueue = [t for t in targets if t["id"] == "hicache_controller.writeback_enqueue_observed"]
                    self.assertEqual(len(enqueue), int(consumer == "hicache_dag_patch"))
                    if enqueue:
                        self.assertEqual(enqueue[0].get("capture_emission_timing", False), diagnostics != "off")

    def test_disabled_python_does_not_claim_runtime_observations(self):
        cfg = {
            "profiling": {
                "enabled": False,
                "channels": ["python_probe"],
                "python_probe": {"consumers": ["hicache_dag_patch"]},
            }
        }
        self.assertEqual(normalize_profiling_config(cfg).to_manifest_fragment()["python_runtime_probes"], [])
        cfg["profiling"].update(enabled=True, channels=["torch"])
        self.assertEqual(normalize_profiling_config(cfg).to_manifest_fragment()["python_runtime_probes"], [])


if __name__ == "__main__":
    unittest.main()
