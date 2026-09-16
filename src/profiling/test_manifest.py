"""Small checks for the profiling-to-modeling workload report handoff."""

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from profiling.manifest import build_profile_manifest
from markov_internal.modeling.workload import discover_workload_window


class WorkloadManifestCheck(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.report = self.root / "bench" / "case" / "workload_report.json"
        self.report.parent.mkdir(parents=True)
        self.report.write_text(json.dumps({"formal_window": {"formal_begin_ms": 100, "formal_end_ms": 150}}))
        self.manifest = self.root / "profile_manifest.json"

    def build_manifest(self):
        return build_profile_manifest(run_dir=self.root, cfg={}, runtime=SimpleNamespace(to_manifest_fragment=lambda: {}),
                                      started_at=0, ended_at=1, status="completed", dry_run=False)

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
        self.assertEqual(discover_workload_window({"workload_report": str(self.report)}, self.manifest).report_path, self.report)

    def test_older_manifest_still_discovers_one_report(self):
        self.manifest.write_text(json.dumps({"run_dir": str(self.root)}))
        self.assertEqual(discover_workload_window({}, self.manifest).report_path, self.report)

    def test_absent_report_is_not_invented(self):
        self.report.unlink()
        manifest = self.build_manifest()
        self.assertEqual(manifest["bench"]["workload_report_files"], [])
        self.manifest.write_text(json.dumps(manifest))
        self.assertIsNone(discover_workload_window({}, self.manifest))

    def test_each_torch_trace_carries_its_own_clock(self):
        for rank in (1, 2):
            trace = self.root / "trace" / "torch" / str(rank) / "trace_view.json"
            trace.parent.mkdir(parents=True)
            trace.write_text("[]")
        clocks = [{"origin_tick": 10}, {"origin_tick": 20}]
        with patch("profiling.manifest.read_host_clock", side_effect=clocks) as read:
            manifest = self.build_manifest()
        entries = manifest["trace"]["torch_trace_files"]
        self.assertEqual([e["host_clock"] for e in entries], clocks)
        self.assertEqual([call.args[0] for call in read.call_args_list], [Path(e["path"]) for e in entries])

    def test_profile_entrypoint_does_not_shadow_standard_library(self):
        entry = Path(__file__).resolve().parents[2] / "scripts/internal/entrypoints/profile.py"
        code = ("import runpy, sys; from pathlib import Path; "
                "entry = Path(sys.argv[1]); sys.path.insert(0, str(entry.parent)); "
                "runpy.run_path(str(entry), run_name='entrypoint_check'); "
                "import cProfile, profile; assert callable(cProfile.run); "
                "assert Path(profile.__file__).resolve() != entry")
        subprocess.run([sys.executable, "-c", code, str(entry)], check=True)


if __name__ == "__main__":
    unittest.main()
