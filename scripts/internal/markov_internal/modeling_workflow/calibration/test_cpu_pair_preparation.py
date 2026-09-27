"""Pair preparation rejects unusable captures before reading timing data."""

from pathlib import Path
import unittest
from unittest.mock import patch

from .cpu_pair_preparation import _capture, prepare_forward_cpu_pair


class CpuPairPreparationTests(unittest.TestCase):
    def test_recorded_run_relative_timing_directory(self):
        root = Path("/tmp/paired-capture")
        with (
            patch(
                "markov_internal.modeling_workflow.calibration.cpu_pair_preparation.load_json",
                side_effect=[
                    {
                        "status": "completed",
                        "config_path": str(root / "config.json"),
                        "bench": {"workload_report_files": [{"path": str(root / "report.json")}]},
                    },
                    {"env": {"SGLANG_STEP_TIMING_DIR": "{run_dir}/timing"}},
                    {"workload_id": "calibration", "forced_token": {"plan_path": str(root / "tokens.json")}},
                    {"requests": []},
                ],
            ),
            patch(
                "markov_internal.modeling_workflow.calibration.cpu_pair_preparation.parse_server_command_flags",
                return_value={"tp_size": "2"},
            ),
        ):
            capture = _capture(root / "profile_manifest.json", 2)
        self.assertEqual(capture["timing_dir"], root / "timing")

    def test_same_capture_cannot_be_its_own_reference(self):
        with self.assertRaises(ValueError):
            prepare_forward_cpu_pair(
                Path("data/base/profile_manifest.json"), Path("data/base/profile_manifest.json"), 2
            )

    def test_failed_or_dry_capture_never_reads_runtime_inputs(self):
        for fields in (
            {"status": "failed"},
            {"status": "completed", "dry_run": True},
            {"status": "completed", "collection_errors": ["missing trace"]},
        ):
            with (
                self.subTest(fields=fields),
                patch(
                    "markov_internal.modeling_workflow.calibration.cpu_pair_preparation.load_json",
                    return_value=fields,
                ) as reader,
                self.assertRaises(ValueError),
            ):
                _capture(Path("data/base/profile_manifest.json"), 2)
            self.assertEqual(reader.call_count, 1)

    def test_rank_count_comes_from_capture_not_observed_rows(self):
        with (
            patch(
                "markov_internal.modeling_workflow.calibration.cpu_pair_preparation.load_json",
                return_value={"status": "completed"},
            ) as reader,
            patch(
                "markov_internal.modeling_workflow.calibration.cpu_pair_preparation.parse_server_command_flags",
                return_value={"tp_size": "4"},
            ),
            self.assertRaises(ValueError),
        ):
            _capture(Path("data/base/profile_manifest.json"), 2)
        self.assertEqual(reader.call_count, 1)
