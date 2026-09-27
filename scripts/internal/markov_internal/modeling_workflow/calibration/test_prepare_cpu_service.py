"""The public service command uses source measurements and the formal source window."""

import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest.mock import patch

from ...modeling.workload import WorkloadWindow
from . import prepare_cpu_service as preparation


class PrepareCpuServiceTests(unittest.TestCase):
    def test_source_only_command_and_measurement_input(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            window = WorkloadWindow(root / "report.json", 100000, 200000, 100000, "workload_report.formal_window")
            with (
                patch.object(preparation, "discover_workload_window", return_value=window),
                patch.object(
                    preparation,
                    "prepare_forward_cpu_pair",
                    return_value={"source_manifest": "base"},
                ) as pair,
                patch.object(preparation, "trace_graph_executable", return_value=Path("trace_graph")),
                patch.object(preparation, "execute_trace_graph") as execute,
            ):
                output = preparation.prepare_cpu_service(root / "light.json", root / "base.json", 4, root)
                pair.assert_called_once_with(root / "light.json", root / "base.json", 4, correct_recorder=False)
                command = execute.call_args.args[0]
                self.assertEqual(command[command.index("--trace-window-start-us") + 1], "100")
                self.assertEqual(command[command.index("--trace-window-end-us") + 1], "200")
                self.assertNotIn("--model-config", command)
                self.assertEqual(output, root / "cpu_service.json")
                self.assertTrue((root / "cpu_measurements.json").is_file())

    def test_missing_formal_window_prevents_preparation(self):
        with (
            patch.object(preparation, "discover_workload_window", return_value=None),
            patch.object(preparation, "prepare_forward_cpu_pair") as pair,
        ):
            with self.assertRaises(ValueError):
                preparation.prepare_cpu_service(Path("light"), Path("full"), 2, Path("output"))
            pair.assert_not_called()

    def test_recorder_option_uses_one_graph_and_only_marks_success_after_binding(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            window = WorkloadWindow(root / "report.json", 0, 30000, 30000, "workload_report.formal_window")
            writes = [dict(pid=1, tid=2, method="scope", method_begin_ns=1000, method_end_ns=3000, thread_cpu_ns=1000)]

            def execute(command):
                measurements = preparation.load_json(root / "cpu_measurements.json")
                self.assertNotIn("preparation", measurements)
                self.assertEqual(measurements["recorder_writes"], writes)
                preparation.write_json(
                    root / "cpu_service.retained.json",
                    {
                        "source_recorder_correction": dict(
                            writes=1, added_integer_reduction_us=1, applied_added_integer_reduction_us=1
                        )
                    },
                )

            with (
                patch.object(preparation, "discover_workload_window", return_value=window),
                patch.object(
                    preparation,
                    "prepare_forward_cpu_pair",
                    side_effect=lambda *a, **kw: {
                        "source_manifest": "base",
                        "host_measurements": {"rows": []},
                        "recorder_writes": writes,
                    },
                ),
                patch.object(preparation, "trace_graph_executable", return_value=Path("trace_graph")),
                patch.object(
                    preparation, "execute_trace_graph", side_effect=RuntimeError("binding failed")
                ) as execution,
            ):
                with self.assertRaisesRegex(RuntimeError, "binding failed"):
                    preparation.prepare_cpu_service(root / "light", root / "full", 2, root, correct_recorder=True)
                self.assertNotIn("preparation", preparation.load_json(root / "cpu_measurements.json"))

                execution.reset_mock()
                execution.side_effect = execute
                preparation.prepare_cpu_service(root / "light", root / "full", 2, root, correct_recorder=True)
                execution.assert_called_once()
            result = preparation.load_json(root / "cpu_measurements.json")
            self.assertEqual(result["preparation"], dict(tp_size=2, correct_recorder=True))
            self.assertEqual(result["source_recorder_correction"]["applied_new_host_reduction_us"], 1)
