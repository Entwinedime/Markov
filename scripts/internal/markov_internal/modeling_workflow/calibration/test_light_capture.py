"""Light replay changes measurement settings, not model or request work."""

import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

from ...common.io import load_json, write_json
from ...common.paths import ROOT_DIR, repo_relative_path
from . import light_capture


class LightCaptureTests(unittest.TestCase):
    def setUp(self):
        self.directory = TemporaryDirectory(dir=ROOT_DIR)
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        (self.root / "step.jsonl").touch()
        self.plan = {"workload_id": "workload", "requests": []}
        write_json(self.root / "original_plan.json", self.plan)
        write_json(self.root / "bundle_plan.json", self.plan)
        write_json(self.root / "bundle.json", {"plans": {"workload": {"path": "bundle_plan.json"}}})
        write_json(
            self.root / "manifest.json",
            {
                "status": "completed",
                "config_path": str(self.root / "config.json"),
                "bench": {"workload_report_files": [{"path": str(self.root / "report.json")}]},
            },
        )
        write_json(
            self.root / "report.json",
            {
                "workload_id": "workload",
                "forced_token": dict(
                    enabled=True,
                    mode="replay",
                    plan_path=str(self.root / "original_plan.json"),
                    request_count=1,
                    output_checked_count=1,
                    mismatch_count=0,
                    unchecked_count=0,
                    prompt_mismatch_count=0,
                    all_actual_outputs_match_plan=True,
                ),
            },
        )
        self.config = dict(
            name="base",
            run_id="original_capture",
            server={"command": ["serve", "--tp", "4"], "startup_max_attempts": 3},
            metadata={"suite_input_id": "workload"},
            env={"SGLANG_STEP_TIMING_DIR": str(self.root), "HOOK_SYNC_THREAD_CPU": "1"},
            profiling={
                "enabled": True,
                "channels": ["torch", "ld_preload", "python_probe"],
                "ld_preload": {"enabled": True},
            },
            bench={
                "command": [
                    "hicache_template_workload.py",
                    "--output-dir",
                    "{bench_dir}/w",
                    "--forced-token-mode",
                    "replay",
                    "--forced-token-plan",
                    str(repo_relative_path(self.root / "original_plan.json")),
                    "--template",
                    "template.json",
                ]
            },
        )

    def render(self, *, bundle=False):
        write_json(self.root / "config.json", self.config)
        return light_capture.light_capture_config(
            self.root / "manifest.json", self.root / "new", self.root / "bundle.json" if bundle else None
        )

    def test_inference_and_request_inputs_unchanged(self):
        original = self.config
        rendered = self.render()
        self.assertEqual(load_json(self.root / "config.json"), original)
        self.assertEqual(rendered["server"], {**original["server"], "startup_max_attempts": 1})
        self.assertEqual(rendered["bench"]["command"], original["bench"]["command"])
        self.assertEqual(rendered["profiling"]["channels"], ["ld_preload"])
        self.assertNotEqual(rendered["env"]["SGLANG_STEP_TIMING_DIR"], original["env"]["SGLANG_STEP_TIMING_DIR"])
        self.assertEqual(self.render(bundle=True), rendered)
        write_json(self.root / "bundle_plan.json", {**self.plan, "requests": [{"origin_input_ids": [2]}]})
        with self.assertRaisesRegex(ValueError, "differs"):
            self.render(bundle=True)

    def test_existing_report_output_cannot_be_overwritten(self):
        self.config["bench"]["command"][2] = "/old/bench/w"
        with self.assertRaises(ValueError):
            self.render()

    def test_ordinary_profile_identity_survives_capture_rename(self):
        del self.config["metadata"]
        rendered = self.render()
        self.assertEqual(rendered["metadata"], {"config_id": "base", "workload_id": "workload"})
        self.assertEqual(rendered["name"], "base_cpu_light")
        self.assertNotIn("metadata", load_json(self.root / "config.json"))

    def test_unmeasured_base_cannot_be_fixed_by_light_capture(self):
        del self.config["env"]["SGLANG_STEP_TIMING_DIR"]
        with self.assertRaises(ValueError):
            self.render()
