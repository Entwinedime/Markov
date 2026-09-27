"""CPU light capture budgets include failed runs and replay success is not a DAG claim."""

import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest.mock import patch

from ..common.io import write_json, load_json
from . import capture as profile_capture, cpu_capture


class CpuCaptureTests(unittest.TestCase):
    def setUp(self):
        self.directory = TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.source = self.root / "base/profile_manifest.json"
        write_json(
            self.source,
            {"bench": {"workload_report_files": [{"path": str(self.source.parent / "bench/w/workload_report.json")}]}},
        )
        write_json(
            self.source.parent / "bench/w/workload_report.json",
            {
                "workload_id": "logical_workload",
                "requests": [{"kind": "request", "origin_input_count": 8, "forced_output_count": 2}],
            },
        )
        self.pair = dict(profile_manifest=str(self.source))
        self.plan = dict(
            capture_root=str(self.root / "captures"),
            forced_token_bundle=None,
            capture_budget=dict(wall_seconds=100, server_starts=1, requests=1, tokens=10),
        )
        for module in (cpu_capture, profile_capture):
            for name in ("require_repo_path", "repo_relative_path"):
                patcher = patch.object(module, name, side_effect=Path)
                patcher.start()
                self.addCleanup(patcher.stop)

    def run_failed_capture(self, row, command, remaining):
        row.update(status="failed", wall_seconds=40)

    def test_budget_prevents_start_and_failed_attempt_is_charged(self):
        self.plan["capture_budget"]["tokens"] = 9
        with patch.object(profile_capture, "run_container_attempt") as run:
            self.assertIsNone(cpu_capture.capture_light_base(self.pair, self.plan, dry_run=True))
            self.assertEqual(self.plan["status"], "cpu_capture_budget_exhausted")
            self.assertEqual(self.plan["capture_required"], dict(server_starts=1, requests=1, tokens=10))
            self.assertFalse((self.root / "captures").exists())
            self.assertIsNone(cpu_capture.capture_light_base(self.pair, self.plan))
            self.assertEqual(self.plan["status"], "cpu_capture_budget_exhausted")
            run.assert_not_called()
        self.plan["capture_budget"]["tokens"] = 10
        with patch.object(profile_capture, "run_container_attempt") as run:
            self.assertIsNone(cpu_capture.capture_light_base(self.pair, self.plan, dry_run=True))
            self.assertEqual(self.plan["status"], "needs_preparation")
            run.assert_not_called()
        with (
            patch.object(cpu_capture, "light_capture_config", side_effect=self.capture_config),
            patch.object(profile_capture, "run_container_attempt", side_effect=self.run_failed_capture),
            patch("markov_internal.modeling_workflow.capture.time.monotonic", side_effect=[100, 142]),
        ):
            self.assertIsNone(cpu_capture.capture_light_base(self.pair, self.plan))
            self.assertEqual(self.plan["status"], "cpu_capture_failed")
        ledger = load_json(self.root / "captures/capture_ledger.json")
        self.assertEqual(ledger["usage"], dict(wall_seconds=42, server_starts=1, requests=1, tokens=10))
        self.assertEqual(self.plan["capture_usage"], ledger["usage"])
        with patch.object(profile_capture, "run_container_attempt") as run:
            self.assertIsNone(cpu_capture.capture_light_base(self.pair, self.plan))
            self.assertEqual(self.plan["status"], "cpu_capture_budget_exhausted")
            run.assert_not_called()

    def test_failed_startup_charges_time_and_start_but_not_unstarted_requests(self):
        def fail(row, command, remaining):
            output = Path(row["config_path"]).parent
            write_json(
                output / "profiles/light/profile_manifest.json",
                dict(status="failed", workload_started=False),
            )
            row["status"] = "failed"

        with (
            patch.object(cpu_capture, "light_capture_config", side_effect=self.capture_config),
            patch.object(profile_capture, "run_container_attempt", side_effect=fail),
            patch.object(profile_capture.time, "monotonic", side_effect=[100, 142]),
        ):
            self.assertIsNone(cpu_capture.capture_light_base(self.pair, self.plan))

        ledger = load_json(self.root / "captures/capture_ledger.json")
        self.assertEqual(ledger["usage"], dict(wall_seconds=42, server_starts=1, requests=0, tokens=0))
        self.assertEqual(ledger["attempts"][0]["status"], "failed")
        self.assertEqual(self.plan["attempt"], ledger["attempts"][0])

    def test_success_is_recorded_and_reused(self):
        def capture(row, command, remaining):
            self.assertNotIn("--forced-token-bundle", command)
            output = Path(row["config_path"]).parent
            manifest = output / "profiles/light/profile_manifest.json"
            write_json(
                manifest,
                dict(
                    status="completed",
                    dry_run=False,
                    bench={"workload_report_files": [{"path": str(manifest.parent / "bench/w/workload_report.json")}]},
                    trace_channel_coverage={"ld_preload_trace_files": 1},
                ),
            )
            write_json(
                manifest.parent / "bench/w/workload_report.json",
                {"status": "completed", "requests": [{"kind": "request"}]},
            )
            (output / "timing").mkdir()
            (output / "timing/step.jsonl").touch()
            row.update(status="completed", wall_seconds=40)
            write_json(output / "profiles/unrelated/profile_manifest.json", {"status": "failed"})

        with (
            patch.object(cpu_capture, "light_capture_config", side_effect=self.capture_config),
            patch(
                "markov_internal.modeling_workflow.capture.forced_token_quality_from_report",
                return_value={"ready": True, "mode": "replay"},
            ),
            patch.object(profile_capture, "run_container_attempt", side_effect=capture) as run,
        ):
            first = cpu_capture.capture_light_base(self.pair, self.plan)
            self.plan["capture_budget"] = None
            second = cpu_capture.capture_light_base(self.pair, self.plan)
            self.assertEqual(first, second)
            self.assertEqual(cpu_capture.capture_light_base(self.pair, self.plan, dry_run=True), first)
            self.assertEqual(run.call_count, 1)

    def capture_config(self, source, output, bundle):
        return dict(run_root=str(output / "profiles"), run_id="light")
