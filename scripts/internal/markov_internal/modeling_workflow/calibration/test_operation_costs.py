import unittest
import subprocess
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
from unittest.mock import patch
from ...common.io import write_json, load_json
from . import operation_costs
from .operation_costs import summarize, FIELDS


class OperationCostsTest(unittest.TestCase):
    def check_program_export(self, operation, mode):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            source, service = root / "manifest.json", root / "cpu.json"
            write_json(service, dict(source_manifest=str(source)))
            report = root / "workload.json"
            write_json(report, dict(requests=[dict(kind="request", start_time_ms=0.001, end_time_ms=0.003)]))

            def export(command, **kwargs):
                self.assertEqual(command[2:4], ["1", "3"])
                self.assertEqual(command[5], mode)
                write_json(
                    Path(command[4]),
                    dict(
                        role="fixed_calibration",
                        source_policy="best_effort",
                        source_manifest=str(source),
                        cpu_service_file=str(service),
                        cost_basis="paired_cpu_service",
                    ),
                )
                return subprocess.CompletedProcess(command, 2 if len(command) > 7 else 0)

            args = [
                "operation_costs",
                "--profile-manifest",
                str(source),
                "--cpu-service-cost",
                str(service),
                "--page-size",
                "128",
                "--output-dir",
                str(root / "output"),
                "--operation",
                operation,
                "--operation",
                operation,
            ]
            with (
                patch("sys.argv", args),
                patch.object(operation_costs, "repo_relative_path", side_effect=Path),
                patch.object(
                    operation_costs,
                    "discover_workload_window",
                    return_value=SimpleNamespace(
                        report_path=report, source="workload_report.formal_window", start_ns=1000, end_ns=2000
                    ),
                ),
                patch.object(
                    operation_costs,
                    "discover_profile_runs",
                    return_value=[SimpleNamespace(hicache_config={"prefetch_policy": "best_effort"})],
                ),
                patch.object(operation_costs.subprocess, "run", side_effect=export) as run,
            ):

                def interrupted(command, **kwargs):
                    write_json(Path(command[4]), {"partial": True})
                    raise RuntimeError("interrupted extraction")

                run.side_effect = interrupted
                with self.assertRaisesRegex(RuntimeError, "interrupted extraction"):
                    operation_costs.main()
                self.assertEqual(list((root / "output").iterdir()), [])

                final = root / "output" / f"{operation}.json"
                write_json(final, {"previous": True})
                with self.assertRaisesRegex(RuntimeError, "interrupted extraction"):
                    operation_costs.main()
                self.assertEqual(load_json(final), {"previous": True})

                run.reset_mock()
                run.side_effect = export
                operation_costs.main()
                run.assert_called_once()

                partial = root / "partial_output"
                args[args.index("--output-dir") + 1] = str(partial)
                args.extend(("--operation", "load_index"))
                with self.assertRaises(subprocess.CalledProcessError):
                    operation_costs.main()
                self.assertEqual([path.name for path in partial.iterdir()], [operation + ".json"])
            self.assertEqual(load_json(root / "output" / f"{operation}.json")["cost_basis"], "paired_cpu_service")
            self.assertEqual([p.name for p in (root / "output").iterdir()], [operation + ".json"])

    def test_prefetch_export_uses_source_policy_and_only_requested_operation(self):
        self.check_program_export("prefetch_wait", "prefetch-wait:best_effort")

    def test_write_export_keeps_program_without_duplicate_audit(self):
        for operation in ("write_host", "release_host", "prefetch_query"):
            with self.subTest(operation=operation):
                self.check_program_export(operation, operation.replace("_", "-"))

    def audit(self):
        return dict(
            source_manifest="independent/profile_manifest.json",
            cpu_service_file="independent/cpu_service.json",
            cost_basis="paired_cpu_service",
            rows=[dict(rank=0, phase="EXTEND", status="observed", **{f: 2 for f in FIELDS})],
        )

    def test_retains_residual_and_device_cost(self):
        audit = self.audit()
        result = summarize(audit, "layer_wait", 128)
        self.assertEqual(result["rows"][0]["main_residual_us"], 2)
        self.assertEqual(result["rows"][0]["device_us"], 2)
        self.assertEqual(result["cpu_service_file"], audit["cpu_service_file"])

    def test_refuses_uncorrected_formal_costs(self):
        audit = self.audit()
        audit["cost_basis"] = "profiled"
        with self.assertRaises(ValueError):
            summarize(audit, "layer_wait", 128)

    def test_no_silent_drop_of_incomplete_rows(self):
        audit = self.audit()
        audit["rows"][0]["status"] = "incomplete"
        with self.assertRaises(ValueError):
            summarize(audit, "layer_wait", 128)

    def test_means_keep_ranks_and_phases_separate(self):
        audit = self.audit()
        audit["rows"].extend([dict(audit["rows"][0], main_us=4), dict(audit["rows"][0], rank=1, main_us=9)])
        result = summarize(audit, "layer_wait", 128)
        self.assertEqual([r["main_us"] for r in result["rows"]], [3, 9])

    def test_empty_rejected(self):
        audit = self.audit()
        audit["rows"] = []
        with self.assertRaises(ValueError):
            summarize(audit, "layer_wait", 128)
