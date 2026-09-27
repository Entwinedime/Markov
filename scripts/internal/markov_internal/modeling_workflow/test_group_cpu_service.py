"""Group preparation binds CPU service to sources, never to individual targets."""

import copy
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
from unittest.mock import patch

from . import group_cpu_service as service
from . import prepare
from .group import ProfileBudget
from ..common.io import load_json, write_json


class GroupCpuServiceTests(unittest.TestCase):
    def _write_inputs(self, inputs):
        write_json(self.group.output_dir / "model_inputs.json", inputs)
        return SimpleNamespace(returncode=0)

    def setUp(self):
        self.directory = TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        root = Path(self.directory.name)
        self.source, self.light = root / "source.json", root / "light.json"
        self.source.touch()
        self.light.touch()
        self.group = SimpleNamespace(
            raw={"output_dir": str(root)},
            sources=[SimpleNamespace(manifest_path=self.source)],
            output_dir=root,
            targets=[],
            path=root / "group.json",
            missing_base_workloads=[],
            physical={},
            base_config="base",
            workload_ids=["workload"],
            budget=ProfileBudget(0, 0, 0, 0),
        )
        for module, name, convert in (
            (service, "require_repo_path", Path),
            (service, "repo_relative_path", Path),
            (prepare, "_relative", str),
        ):
            patcher = patch.object(module, name, side_effect=convert)
            patcher.start()
            self.addCleanup(patcher.stop)
        self.pair = dict(profile_manifest=str(self.source), light_manifest=str(self.light), tp_size=2)

    def test_optional_and_complete_source_coverage(self):
        self.assertEqual(service.cpu_service_plan(self.group)["status"], "not_requested")
        self.group.raw["cpu_service_pairs"] = [self.pair]
        self.assertEqual(len(service.cpu_service_plan(self.group)["pairs"]), 1)
        self.group.raw["cpu_service_pairs"].append(copy.deepcopy(self.pair))
        with self.assertRaises(ValueError):
            service.cpu_service_plan(self.group)
        self.group.raw["cpu_service_pairs"].pop()
        self.group.sources.append(SimpleNamespace(manifest_path=self.light))
        with self.assertRaises(ValueError):
            service.cpu_service_plan(self.group)

    def test_existing_source_service_is_reused_without_repreparing(self):
        path = self.group.output_dir / "service.json"
        write_json(path, {"source_manifest": str(self.source)})
        self.group.raw["cpu_service_costs"] = [str(path)]
        self.group.raw["cpu_service_capture_budget"] = {
            "wall_seconds": 0,
            "server_starts": 0,
            "requests": 0,
            "tokens": 0,
        }
        plan = service.cpu_service_plan(self.group)
        with patch.object(service.subprocess, "run") as run:
            self.assertEqual(service.prepare_group_cpu_service(plan, dry_run=True), [path])
            run.assert_not_called()
        self.assertEqual(plan["status"], "prepared")
        self.group.raw["cpu_service_pairs"] = [self.pair]
        with self.assertRaises(ValueError):
            service.cpu_service_plan(self.group)

    def test_missing_light_requires_budget_but_reuses_base_token_input(self):
        self.group.raw["cpu_service_pairs"] = [
            {key: value for key, value in self.pair.items() if key != "light_manifest"}
        ]
        plan = service.cpu_service_plan(self.group)
        self.assertEqual(plan["status"], "planned")  # Ledger reuse is resolved before requiring a new budget.
        self.group.raw.update(
            cpu_service_capture_budget={"wall_seconds": 100, "server_starts": 1, "requests": 1, "tokens": 10},
        )
        del self.group.raw["cpu_service_pairs"]
        self.group.sources[0].run_dir = self.group.output_dir
        with patch.object(service, "parse_server_command_flags", return_value={"tp_size": "2"}):
            plan = service.cpu_service_plan(self.group)
        self.assertEqual(plan["pairs"][0]["profile_manifest"], str(self.source))
        self.assertEqual(plan["pairs"][0]["tp_size"], 2)
        with (
            patch.object(service, "capture_light_base", return_value=self.light) as capture,
            patch.object(service.subprocess, "run") as run,
            patch.object(service, "cpu_service_inputs") as bind,
        ):
            outputs = service.prepare_group_cpu_service(plan)
        self.assertEqual(capture.call_count, 1)
        self.assertEqual(plan["pairs"][0]["light_manifest"], str(self.light))
        self.assertIn("--correct-recorder", run.call_args.args[0])
        bind.assert_called_once_with(outputs, (self.source,))
        self.assertEqual(plan["status"], "prepared")

        self.group.raw["cpu_service_pairs"] = [
            {key: value for key, value in self.pair.items() if key != "light_manifest"}
        ]
        for status in ("cpu_capture_budget_exhausted", "cpu_capture_failed", "capture_incomplete"):
            plan = service.cpu_service_plan(self.group)

            def stop_capture(pair, current_plan, *, dry_run=False):
                current_plan["status"] = status

            with (
                self.subTest(status=status),
                patch.object(prepare, "cpu_service_plan", return_value=plan),
                patch.object(service, "capture_light_base", side_effect=stop_capture),
                patch.object(service.subprocess, "run") as run,
            ):
                result = prepare.prepare(self.group, dry_run=False)

            run.assert_not_called()
            self.assertEqual(result["status"], status)
            self.assertEqual(result["predictions_completed"], 0)
            self.assertEqual(load_json(self.group.output_dir / "group_summary.json")["status"], status)

    def test_reuse_requires_matching_capture_and_preparation(self):
        self.group.raw["cpu_service_pairs"] = [self.pair]
        plan = service.cpu_service_plan(self.group)
        output = Path(plan["pairs"][0]["output_dir"]) / "cpu_service.json"
        output.parent.mkdir(parents=True)
        write_json(output, {})
        measurements = dict(
            source_manifest=str(self.source),
            light_manifest=str(self.light),
            preparation=dict(workload="old_directory_label", tp_size=2, correct_recorder=True),
        )
        write_json(output.parent / "cpu_measurements.json", measurements)
        with patch.object(service.subprocess, "run") as run, patch.object(service, "cpu_service_inputs") as bind:
            self.assertEqual(service.prepare_group_cpu_service(plan, dry_run=True), [output])
            run.assert_not_called()
            bind.assert_called_once()
        for field, value in [("tp_size", 4), ("correct_recorder", False)]:
            changed = copy.deepcopy(measurements)
            changed["preparation"][field] = value
            write_json(output.parent / "cpu_measurements.json", changed)
            with patch.object(service.subprocess, "run") as run, patch.object(service, "cpu_service_inputs"):
                service.prepare_group_cpu_service(plan)
                run.assert_called_once()

    def test_dry_run_does_not_build_missing_cpu_service(self):
        self.group.raw["cpu_service_pairs"] = [self.pair]
        plan = service.cpu_service_plan(self.group)
        with patch.object(service.subprocess, "run") as run, patch.object(service, "capture_light_base") as capture:
            self.assertEqual(service.prepare_group_cpu_service(plan, dry_run=True), [])
        run.assert_not_called()
        capture.assert_not_called()
        self.assertEqual(plan["status"], "needs_preparation")

    def test_prediction_receives_one_shared_source_service(self):
        output = self.group.output_dir / "cpu_service.json"
        summary = dict(
            cpu_service={"status": "prepared", "pairs": []}, cpu_service_files=[str(output)], prediction_count=2
        )
        targets = [SimpleNamespace(label=name, fields={}) for name in ("one", "two")]
        self.group.targets = targets

        def execute(command, **kwargs):
            self.assertEqual(command[1], "predict-hicache")
            if completed is not None:
                path = self.group.output_dir / "predictions" / "workflow_summary.json"
                write_json(
                    path,
                    {
                        "prediction": {
                            "completed_count": completed,
                            "status": "EXECUTED" if completed == 2 else "CHECK",
                            "cells": [],
                        }
                    },
                )
            return SimpleNamespace(returncode=code)

        for code, completed, status in [
            (0, 2, "predicted"),
            (2, 1, "prediction_incomplete"),
            (1, None, "prediction_failed"),
        ]:
            with (
                self.subTest(code=code),
                patch.object(prepare.subprocess, "run", side_effect=execute) as run,
            ):
                current = dict(summary)
                prepare.predict_group(self.group, current, model_run_jobs=1, diagnostics="off")
            run.assert_called_once()
            command = run.call_args.args[0]
            self.assertEqual(command.count("--cpu-service-cost"), 1)
            self.assertEqual(command.count("--target-config"), 2)
            self.assertEqual(current["status"], status)
            if completed is not None:
                self.assertEqual(current["predictions_completed"], completed)
                self.assertNotIn("cells", current["prediction"])
            else:
                self.assertNotIn("prediction", current)  # The previous attempt's file still exists.

    def test_new_shared_measurement_resumes_prediction_without_retrying_old_evidence(self):
        from .calibration import eviction_cpu

        need = {"component": "execution_control/eviction_locked_candidate"}
        cases = [
            ({"status": "cpu_primitive_measured", "reused": False}, False, "predicted", 1),
            ({"status": "cpu_primitive_measured", "reused": False}, True, "needs_calibration_data", 1),
            ({"status": "cpu_primitive_measured", "reused": True}, False, "prediction_incomplete", 0),
            ({"status": "physical_budget_exhausted"}, False, "prediction_incomplete", 0),
        ]
        for acquired, fail_rebuild, expected_status, expected_builds in cases:
            builds = 0

            def build(*args, **kwargs):
                nonlocal builds
                builds += 1
                return {"status": "data_limitation" if fail_rebuild else "ready"}

            def predict(group, summary, **kwargs):
                completed = builds == 1
                summary.update(
                    status="predicted" if completed else "prediction_incomplete",
                    prediction={"missing_costs": [] if completed else [need]},
                )

            summary = {"prediction_count": 1}
            with (
                self.subTest(acquired=acquired, fail_rebuild=fail_rebuild),
                patch.object(prepare, "_predict", side_effect=predict),
                patch.object(prepare, "_scan_model_inputs", side_effect=build),
                patch.object(prepare.GroupRequest, "load", return_value=self.group),
                patch.object(eviction_cpu, "acquire_eviction_cpu", return_value=acquired) as acquire,
            ):
                prepare.predict_group(self.group, summary, model_run_jobs=1, diagnostics="off")

            self.assertEqual(summary["status"], expected_status)
            self.assertEqual(builds, expected_builds)
            acquire.assert_called_once_with(self.group, [need])
            if fail_rebuild:
                self.assertNotIn("prediction", summary)
                self.assertEqual(summary["predictions_completed"], 0)

    def test_execution_acquires_only_requested_missing_services(self):
        for requested, evidence_gap, unused, partial in (
            ("physical/load", None, "physical/prefetch_stages", False),
            ("physical/load", "physical/load", "physical/prefetch_stages", True),
            (
                "service/write_host_to_storage_new",
                "service/write_host_to_storage_new",
                "physical/write_host_to_storage_existing",
                False,
            ),
            (
                "physical/write_host_to_storage_existing",
                "service/write_host_to_storage_existing",
                "service/write_host_to_storage_new",
                False,
            ),
        ):
            summary = {
                "model_inputs": {"service_gaps": [{"component": name} for name in (evidence_gap, unused) if name]}
            }

            def predict(group, summary, **kwargs):
                summary.update(status="prediction_incomplete", prediction={"missing_costs": [{"component": requested}]})

            with (
                self.subTest(requested=requested, partial=partial),
                patch.object(prepare, "_predict", side_effect=predict) as predict_call,
                patch.object(
                    prepare,
                    "capture_physical",
                    return_value={"status": "physical_budget_exhausted", "completed_components": [requested]}
                    if partial
                    else {"status": "physical_captured"},
                ) as acquire,
                patch.object(prepare.GroupRequest, "load", return_value=self.group),
                patch.object(prepare, "_scan_model_inputs", return_value={"status": "ready"}),
            ):
                prepare.predict_group(self.group, summary, model_run_jobs=1, diagnostics="off")

            acquire.assert_called_once_with(self.group, dry_run=False, required_components={requested})
            self.assertEqual(predict_call.call_count, 2)
            self.assertEqual(summary["status"], "prediction_incomplete")

    def test_execution_control_gap_uses_one_shared_paired_capture(self):
        for programs, declared_release, stopped, physical_exhausted in (
            ([None], False, False, False),
            (["best_effort"], False, False, False),
            (["local_return"], False, False, False),
            (["wait_complete"], False, False, False),
            (["best_effort", "wait_complete"], True, False, False),
            (["best_effort", "wait_complete"], False, True, False),
            ([None, "best_effort", "wait_complete"], False, True, False),
            (["best_effort", "wait_complete"], False, False, True),
        ):
            needs = [
                {"component": "execution_control/prefetch", "coordinates": [{"program": program}]}
                if program
                else {"component": "execution_control/release_regular"}
                for program in programs
            ]
            self.group.raw["control_calibrations"] = {"release_host": "declared.json"} if declared_release else {}
            if declared_release:
                needs.append({"component": "execution_control/release_regular"})
            if physical_exhausted:
                needs.extend(
                    [
                        {"component": "physical/load"},
                        {"component": "execution_control/eviction_locked_candidate"},
                    ]
                )
            summary = {}

            def predict(*args, **kwargs):
                summary.update(status="prediction_incomplete", prediction={"missing_costs": needs})

            with (
                self.subTest(programs=programs, physical_exhausted=physical_exhausted, stopped=stopped),
                patch.object(prepare, "_predict", side_effect=predict) as run,
                patch.object(
                    prepare, "materialize_fixed_calibration", return_value={"point": {"id": "shared"}}
                ) as plan,
                patch.object(
                    prepare,
                    "prepare_control_calibration",
                    side_effect=[
                        {"status": "budget_exhausted" if stopped and index == 1 else "control_calibrated"}
                        for index in range(len(programs))
                    ],
                ) as capture,
                patch.object(prepare, "capture_physical", return_value={"status": "physical_budget_exhausted"}),
                patch.object(prepare.GroupRequest, "load", return_value=self.group),
                patch.object(prepare, "_scan_model_inputs", return_value={"status": "ready"}) as rebuild,
            ):
                prepare.predict_group(self.group, summary, model_run_jobs=1, diagnostics="off")
            self.assertEqual(run.call_count, 2)
            rebuild.assert_called_once()
            executed_programs = programs
            self.assertEqual(capture.call_count, len(executed_programs))
            self.assertEqual(
                [call.kwargs for call in plan.call_args_list],
                [
                    {"prefetch_policy": "best_effort" if program == "local_return" else program}
                    if program
                    else {"release_only": True}
                    for program in executed_programs
                ],
            )
            self.assertEqual(summary["prediction"]["missing_costs"], needs)
            self.assertEqual(
                [row["operation"] for row in summary["captures"]],
                (["io_service", "eviction_locked_candidate"] if physical_exhausted else [])
                + [program or "release_regular" for program in executed_programs],
            )
            self.assertEqual(summary["captures"][2 if physical_exhausted else 0]["status"], "control_calibrated")
            if stopped:
                self.assertEqual(summary["captures"][1]["status"], "budget_exhausted")

    def test_unavailable_physical_acquisition_allows_later_workload_gaps(self):
        # Each successful shared capture can expose another execution branch.
        # A missing declaration or depleted ledger must not terminate that
        # workload chain. A failed process or incomplete cleanup must stop it.
        requirements = [
            [
                {"component": "physical/load"},
                {"component": "execution_control/prefetch", "coordinates": [{"program": "best_effort"}]},
            ],
            [
                {"component": "physical/prefetch_stages"},
                {"component": "execution_control/prefetch", "coordinates": [{"program": "wait_complete"}]},
            ],
            [{"component": "physical/prefetch_stages"}],
        ]
        for status in (
            "physical_budget_exhausted",
            "needs_physical_calibration",
            "physical_budget_required",
            "failed",
            "cleanup_incomplete",
        ):
            summary = {}
            rounds = iter(requirements)

            def predict(*args, **kwargs):
                summary.update(status="prediction_incomplete", prediction={"missing_costs": next(rounds)})

            with (
                self.subTest(status=status),
                patch.object(prepare, "_predict", side_effect=predict),
                patch.object(prepare, "capture_physical", return_value={"status": status}) as physical,
                patch.object(prepare, "materialize_fixed_calibration", return_value={}) as plan,
                patch.object(
                    prepare, "prepare_control_calibration", return_value={"status": "control_calibrated"}
                ) as capture,
                patch.object(prepare.GroupRequest, "load", return_value=self.group),
                patch.object(prepare, "_scan_model_inputs", return_value={"status": "ready"}) as rebuild,
            ):
                prepare.predict_group(self.group, summary, model_run_jobs=1, diagnostics="off")

            if status in {"failed", "cleanup_incomplete"}:
                physical.assert_called_once()
                capture.assert_not_called()
                rebuild.assert_not_called()
                self.assertEqual(summary["prediction"]["missing_costs"], requirements[0])
                continue

            self.assertEqual(physical.call_count, 2)
            self.assertEqual(capture.call_count, 2)
            self.assertEqual(rebuild.call_count, 2)
            self.assertEqual(
                [call.kwargs for call in plan.call_args_list],
                [{"prefetch_policy": "best_effort"}, {"prefetch_policy": "wait_complete"}],
            )
            self.assertEqual(summary["prediction"]["missing_costs"], [{"component": "physical/prefetch_stages"}])

    def test_ready_costs_do_not_trigger_fixed_profile_quota(self):
        self.group.physical = {}
        self.group.budget = ProfileBudget(wall_seconds=100, server_starts=4, requests=10, tokens=100)
        for evidence, dry_run in [("ready", False), ("ready", True), ("data_limitation", True)]:
            with (
                self.subTest(evidence=evidence, dry_run=dry_run),
                patch.object(
                    prepare.subprocess, "run", side_effect=lambda *a, **kw: self._write_inputs({"status": evidence})
                ),
                patch.object(prepare, "capture_calibration") as capture,
            ):
                result = prepare.prepare(
                    self.group,
                    dry_run=dry_run,
                    calibration_only=True,
                )
            capture.assert_not_called()
            if evidence == "ready":
                self.assertEqual(result["status"], "ready_for_model_build")
                self.assertEqual(result["fixed_calibration"]["status"], "not_required")
            else:
                self.assertEqual(result["status"], "no_suitable_experiment")
        definition = {
            "status": "needs_calibration",
            "remaining_profile_count": 4,
            "definition": {"point": {"id": "needed-point"}},
        }
        for plan_status, capture_status in (
            ("needs_calibration", "captured"),
            ("needs_calibration", "budget_exhausted"),
            ("needs_calibration", "invalid_capture_output"),
        ):
            scan_results = iter([{"status": "data_limitation"}, {"status": "ready"}])
            definition["status"] = plan_status
            with (
                self.subTest(plan_status=plan_status, capture_status=capture_status),
                patch.object(prepare, "plan_fixed_calibration", side_effect=[definition, {"status": "not_required"}]),
                patch.object(
                    prepare.subprocess,
                    "run",
                    side_effect=lambda *a, **kw: self._write_inputs(next(scan_results)),
                ),
                patch.object(prepare, "capture_calibration", return_value={"status": capture_status}) as capture,
                patch.object(prepare.GroupRequest, "load", return_value=self.group),
            ):
                result = prepare.prepare(self.group, dry_run=False, calibration_only=True)

            capture.assert_called_once_with(self.group, definition["definition"])
            expected = "ready_for_model_build" if capture_status == "captured" else capture_status
            self.assertEqual(result["status"], expected)
            self.assertEqual(load_json(self.group.output_dir / "group_summary.json")["status"], expected)

    def test_phase_capture_stops_when_evidence_does_not_improve(self):
        missing = [{"component": "phase", "reason": "insufficient independent token coordinates"}]
        with (
            patch.object(
                prepare, "plan_fixed_calibration", return_value=dict(status="needs_calibration", definition={})
            ),
            patch.object(prepare, "_scan_model_inputs", return_value=dict(status="data_limitation", missing=missing)),
            patch.object(prepare.GroupRequest, "load", return_value=self.group),
            patch.object(prepare, "capture_calibration", return_value={"status": "captured"}) as acquire,
        ):
            result = prepare.prepare(self.group, dry_run=False, calibration_only=True)

        self.assertEqual(result["status"], "experiment_exhausted")
        self.assertEqual(result["fixed_calibration"]["missing"], missing)
        acquire.assert_called_once()

    def test_failed_prepare_keeps_stage_results_and_inclusive_attempt_time(self):
        for error in (RuntimeError("scan failed"), KeyboardInterrupt()):
            with (
                self.subTest(error=type(error).__name__),
                patch.object(prepare.subprocess, "run", side_effect=error),
            ):
                with self.assertRaises(type(error)) as caught:
                    prepare.prepare(self.group, dry_run=False)
            self.assertIs(caught.exception, error)
            summary = load_json(self.group.output_dir / "group_summary.json")
            self.assertEqual(summary["cpu_service"]["status"], "not_requested")
            self.assertEqual(summary["base_manifests"], [str(self.source)])
            self.assertEqual(summary["workloads"], ["workload"])
            attempt = summary["preparation_attempts"][-1]
            self.assertEqual(
                summary["status"], "interrupted" if isinstance(error, KeyboardInterrupt) else "prepare_failed"
            )
            self.assertEqual(attempt["status"], summary["status"])
            self.assertEqual(attempt["failed_stage"], "preparing_model")
            self.assertEqual(summary["prepare_wall_seconds"], attempt["wall_seconds"])
            self.assertGreaterEqual(attempt["wall_seconds"], 0)
        missing = {"status": "data_limitation", "missing": [{"component": "phase"}]}
        with (
            patch.object(prepare.subprocess, "run", side_effect=lambda *a, **kw: self._write_inputs(missing)),
            patch.object(prepare, "plan_fixed_calibration", side_effect=ValueError("no usable token inputs")),
        ):
            with self.assertRaises(ValueError):
                prepare.prepare(self.group, dry_run=False)
        summary = load_json(self.group.output_dir / "group_summary.json")
        self.assertEqual(summary["model_inputs"], missing)
        self.assertEqual(len(summary["preparation_attempts"]), 3)
