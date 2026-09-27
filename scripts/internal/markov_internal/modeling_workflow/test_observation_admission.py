"""Calibration admission uses source/environment evidence, not fixed experiment shape."""

import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
from unittest.mock import patch

from ..common.io import write_json
from ..common.paths import ROOT_DIR, repo_relative_path
from . import observations, group as group_module


class ObservationAdmissionTests(unittest.TestCase):
    def test_incomplete_base_observations_are_replaced_then_reused(self):
        self.group.fixed_calibration_manifests = []
        incomplete = dict(self.cached_base)
        incomplete["source_io_observations"] = {}
        write_json(self.group.output_dir / "observations.json", {"captures": [incomplete]})

        complete = self.cached_base
        with (
            patch.object(observations, "scan_observations", return_value=complete) as scan,
            patch.object(observations, "prepare_model", return_value={"status": "data_limitation"}),
        ):
            for expected_reused in (0, 1):
                result = observations.scan_group(self.group)
                self.assertEqual(result["base_observations_reused"], expected_reused)
            scan.assert_called_once()

    def setUp(self):
        self.directory = TemporaryDirectory(dir=ROOT_DIR)
        self.addCleanup(self.directory.cleanup)
        root = Path(self.directory.name)
        self.base = SimpleNamespace(manifest_path=root / "base.json")
        self.extra = SimpleNamespace(
            manifest_path=root / "extra.json",
            run_dir=root / "extra" / "run",
            hicache_config={"page_size": 96, "write_policy": "write_through", "prefetch_policy": "timeout"},
        )
        self.group = SimpleNamespace(
            output_dir=root,
            base_config="base",
            sources=[self.base],
            fixed_calibration_manifests=[self.extra.manifest_path],
        )
        self.cached_base = dict(
            role="base",
            source_manifest=str(self.base.manifest_path),
            cpu_service_cost=None,
            source_io_observations={"cpu_cost_basis": "profiled"},
        )
        write_json(root / "observations.json", {"captures": [self.cached_base]})

    def test_ledger_accepts_different_experiments_without_regeneration(self):
        # No physical model, token bundle or profiling suite is needed to read this ledger.
        write_json(
            self.group.output_dir / "capture_ledger.json",
            dict(
                base_config="base",
                attempts=[
                    dict(
                        status="completed",
                        mode="capture",
                        profile_captured=True,
                        profile_manifest=str(self.extra.manifest_path),
                        calibration_inputs={"template": "retired-template"},
                    ),
                    dict(status="failed", mode="capture", profile_manifest="failed.json"),
                ],
            ),
        )
        self.assertEqual(observations._calibration_inputs(self.group), {self.extra.manifest_path: None})

    def test_base_cannot_be_relabelled_as_independent(self):
        self.group.fixed_calibration_manifests = [self.base.manifest_path]
        with self.assertRaisesRegex(ValueError, "not independent"):
            observations._calibration_inputs(self.group)

    def test_replay_costs_require_correction_and_invalidate_raw_cache(self):
        self.group.fixed_calibration_manifests = []
        row = dict(
            status="completed",
            mode="replay",
            stage="profiled_replay",
            profile_captured=True,
            profile_manifest=str(self.extra.manifest_path),
        )
        ledger = self.group.output_dir / "capture_ledger.json"
        write_json(ledger, dict(base_config="base", attempts=[row]))
        self.assertEqual(observations._calibration_inputs(self.group), {})
        service = self.group.output_dir / "cpu.json"
        row["cpu_service_cost"] = str(service)
        failed = {**row, "status": "failed", "cpu_service_cost": str(self.group.output_dir / "failed_cpu.json")}
        write_json(ledger, dict(base_config="base", attempts=[row, failed]))
        corrected = dict(
            role="calibration",
            source_manifest=str(repo_relative_path(self.extra.manifest_path)),
            cpu_service_cost=str(repo_relative_path(service)),
        )
        write_json(
            self.group.output_dir / "observations.json",
            {"captures": [self.cached_base, {**corrected, "cpu_service_cost": None}]},
        )
        with (
            patch.object(observations, "discover_profile_runs", return_value=[self.extra]),
            patch.object(observations, "source_environment", return_value={}),
            patch.object(observations, "scan_observations", return_value=corrected) as scan,
            patch.object(observations, "prepare_model", return_value={}),
        ):
            result = observations.scan_group(self.group)
            self.assertEqual(result["calibration_observations_reused"], 0)
            self.assertEqual(scan.call_args.kwargs["cpu_service"], service)
            result = observations.scan_group(self.group)
            self.assertEqual(result["calibration_observations_reused"], 1)
            self.assertEqual(scan.call_count, 1)

    def test_nonendpoint_policy_is_admitted_but_environment_must_match(self):
        cases = [
            ([{"env": {}}, {"env": {}}], True),
            ([{"tp": 2, "env": {}}, {"tp": 4, "env": {}}], False),
            ([{"env": {"SGLANG_STEP_TIMING_DIR": "base"}}, {"env": {"SGLANG_STEP_TIMING_DIR": "calibration"}}], True),
            ([{"env": {"SGLANG_STEP_TIMING_DIR": "base"}}, {"env": {}}], False),
        ]
        for environments, accepted in cases:
            write_json(self.group.output_dir / "observations.json", {"captures": [self.cached_base]})
            with (
                self.subTest(accepted=accepted),
                patch.object(observations, "discover_profile_runs", return_value=[self.extra]),
                patch.object(observations, "source_environment", side_effect=environments),
                patch.object(observations, "scan_observations", return_value={"role": "calibration"}) as scan,
                patch.object(observations, "prepare_model", return_value={}),
            ):
                if accepted:
                    result = observations.scan_group(self.group)
                    self.assertEqual(result["calibration_observations_reused"], 0)
                    scan.assert_called_once()
                else:
                    with self.assertRaisesRegex(ValueError, "runtime resources"):
                        observations.scan_group(self.group)
                    scan.assert_not_called()

    def test_other_base_ledger_is_rejected(self):
        write_json(self.group.output_dir / "capture_ledger.json", dict(base_config="other", attempts=[]))
        with self.assertRaisesRegex(ValueError, "another base"):
            observations._calibration_inputs(self.group)

    def test_extraction_keeps_distinct_windows_without_persistent_summaries(self):
        from ..modeling.workload import WorkloadWindow

        root = self.group.output_dir
        report = root / "report.json"
        window = WorkloadWindow(report, 1_000_000, 2_000_000, 1_000_000, "formal")
        self.extra.workload_window = window
        for role, start, end, calls in (("base", 0, 3, 1), ("calibration", 1, 2, 1), ("calibration", 0, 3, 2)):
            write_json(report, {"requests": [{"kind": "request", "start_time_ms": start, "end_time_ms": end}]})

            def execute(command):
                self.assertIn("--source-observations-only", command)
                begin = int(command[command.index("--trace-window-start-us") + 1])
                io = {"start_us": begin}
                write_json(
                    Path(command[command.index("--run-summary") + 1]),
                    {"source_io_observations": io, "source_phase_observations": {"start_us": begin}},
                )

            with (
                self.subTest(role=role, start=start),
                patch.object(observations, "trace_graph_executable", return_value=Path("/bin/trace_graph")),
                patch.object(observations, "execute_trace_graph", side_effect=execute) as run,
            ):
                result = observations.scan_observations(self.extra, root, role=role)
                self.assertEqual(run.call_count, calls)
            self.assertEqual(result["source_phase_observations"]["start_us"], 1000)
            self.assertEqual(
                result["source_io_observations"]["start_us"], start * 1000 if role == "calibration" else 1000
            )
            self.assertNotIn("summary_path", result)
            self.assertFalse(list(root.glob("observations_*")))

    def test_shared_control_sources_use_the_profile_environment_boundary(self):
        asset = self.group.output_dir / "control.json"
        write_asset = self.group.output_dir / "write.json"
        document = {"source_manifest": str(self.extra.manifest_path), "source_policy": "best_effort"}
        write_json(asset, document)
        write_json(write_asset, {"source_manifest": str(self.extra.manifest_path), "cost_basis": "corrected"})
        self.group.raw = {
            "control_calibrations": {
                "prefetch_wait": {"best_effort": str(asset)},
                "write_host": {"128": {"write_back": str(write_asset)}},
            }
        }
        with (
            patch.object(group_module, "discover_profile_runs", return_value=[self.extra]) as profiles,
            patch.object(group_module, "source_environment", return_value={"env": {}}),
        ):
            evidence = group_module._control_sources(self.group, {"env": {}})
            self.assertEqual(len(evidence), 2)
            self.assertEqual(evidence[str(asset)]["cost_basis"], "source_profile_uncorrected")
            self.assertEqual(evidence[str(write_asset)]["cost_basis"], "corrected")
            profiles.assert_called_once()

            self.group.raw["control_calibrations"]["prefetch_wait"] = {"timeout": str(asset)}
            with self.assertRaisesRegex(ValueError, "declared program"):
                group_module._control_sources(self.group, {"env": {}})

            document["source_policy"] = "wait_complete"
            write_json(asset, document)
            self.assertEqual(group_module._control_sources(self.group, {"env": {}}), evidence)

        with (
            patch.object(group_module, "discover_profile_runs", return_value=[self.extra]),
            patch.object(group_module, "source_environment", return_value={"tp": 4}),
            self.assertRaisesRegex(ValueError, "runtime resources"),
        ):
            group_module._control_sources(self.group, {"tp": 2})
        write_json(asset, {**document, "source_manifest": str(self.base.manifest_path)})
        with self.assertRaisesRegex(ValueError, "not independent"):
            group_module._control_sources(self.group, {"env": {}})
