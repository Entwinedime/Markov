"""Different experiments share limits without sharing measurements by point name."""

import json
from dataclasses import replace
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
from unittest.mock import patch

from ..common.io import load_json, write_json
from ..common.paths import ROOT_DIR, repo_relative_path
from ..modeling.workload import discover_workload_window
from ..profiling.suite import expand_suite
from . import capture, fixed_calibration, control_acquisition
from . import base_capture
from . import control_calibrations as controls
from . import group as group_module


class CalibrationCaptureTests(unittest.TestCase):
    def test_base_acquisition_accepts_ordinary_experiments_and_preserves_overrides(self):
        suite = load_json(ROOT_DIR / "configs/experiments/hicache_manual_workload/profiling_full_dag_replay.json")
        experiment = expand_suite(suite)[0]
        original_labels = experiment.pop("metadata")
        base, workload = original_labels["suite_server_id"], original_labels["suite_input_id"]
        experiment["metadata"] = dict(config_id=base, workload_id=workload)
        experiment.pop("id")

        with TemporaryDirectory(dir=ROOT_DIR) as directory:
            root = Path(directory)
            suite_path, request = root / "experiment.json", root / "group.json"
            write_json(suite_path, experiment)
            write_json(
                request,
                dict(
                    profile_suite=str(suite_path),
                    base_config=base,
                    workload_ids=[workload],
                    target_configs=[base],
                    output_dir=str(root / "output"),
                ),
            )
            group = group_module.GroupRequest.load(request)
            preview = base_capture.capture_base(group, dry_run=True)
            self.assertEqual(group.missing_base_workloads, (workload,))
            self.assertEqual(preview["base_jobs"][0]["modes"], ["capture", "replay"])
            self.assertGreater(preview["base_jobs"][0]["tokens"], 0)
            self.assertEqual(load_json(suite_path), experiment)

            explicit = json.loads(json.dumps(experiment).replace("{forced_token_plan}", "base_tokens.json"))
            write_json(suite_path, explicit)
            replay = base_capture.capture_base(group, dry_run=True)
            self.assertEqual(replay["base_jobs"][0]["modes"], ["replay"])
            self.assertEqual(replay["base_jobs"][0]["tokens"] * 2, preview["base_jobs"][0]["tokens"])

            # Target shorthand reads the fully expanded command, including
            # experiment overrides, never the original matrix server axis.
            changed = dict(
                experiment,
                experiments=[
                    dict(
                        id="only",
                        server=dict(
                            command="python -m sglang.launch_server --enable-hierarchical-cache --page-size 64"
                        ),
                    )
                ],
            )
            write_json(suite_path, changed)
            self.assertEqual(group_module.GroupRequest.load(request).targets[0].fields["page_size"], 64)
            changed["experiments"].append(dict(id="ambiguous", server=experiment["server"]))
            write_json(suite_path, changed)
            with self.assertRaisesRegex(ValueError, "different experiment configurations"):
                group_module.GroupRequest.load(request)

    def test_ledger_rejects_exceptional_completion_without_erasing_primary_failure(self):
        with TemporaryDirectory() as directory:
            path = Path(directory) / "ledger.json"
            for status, expected in (
                ("running", "interrupted"),
                ("completed", "invalid_capture_output"),
                ("cleanup_incomplete", "cleanup_incomplete"),
            ):
                row = dict(reserved_requests=2, reserved_tokens=10, wall_seconds=5)
                with (
                    self.subTest(status=status),
                    patch.object(capture.time, "monotonic", side_effect=[100, 107]),
                    self.assertRaisesRegex(RuntimeError, "validation failed"),
                ):
                    with capture.recorded_capture(path, {"attempts": []}, row, capture.profile_usage, kind="test"):
                        row.update(status=status, error="primary failure")
                        raise RuntimeError("validation failed")

                ledger = load_json(path)
                self.assertEqual(ledger["attempts"][0]["status"], expected)
                self.assertEqual(ledger["usage"], dict(wall_seconds=7, server_starts=1, requests=2, tokens=10))
                if status == "cleanup_incomplete":
                    self.assertEqual(ledger["attempts"][0]["error"], "primary failure")

    def test_pressure_workload_has_distinct_prompts_decode_and_complete_budget(self):
        from ..workload_template.expand import expand_template
        from ..workload_template.schema import load_template

        group = SimpleNamespace(
            physical={"storage_batch_pages": 128}, sources=[SimpleNamespace(hicache_config={"page_size": 128})]
        )
        with patch.object(fixed_calibration, "_base_tokens", return_value=([1, 2], [4096], [])):
            template, fields = fixed_calibration._workload(group)
        prompts = [row["prompt_token_ids"] for row in template["request_defs"].values()]
        self.assertGreater(len(prompts), 4)
        self.assertEqual(len({tuple(row[:128]) for row in prompts}), len(prompts))
        self.assertTrue(all(set(row) <= {1, 2} for row in prompts))
        with TemporaryDirectory() as directory:
            path = Path(directory) / "workload.json"
            write_json(path, template)
            plan = expand_template(load_template(path), None)
            self.assertTrue(all(request.max_new_tokens == 2 for request in plan.requests))

        self.assertTrue(template["defaults"]["sampling"]["ignore_eos"])
        self.assertEqual(fields["point"]["page_size"], 128)
        measured = [step for step in template["steps"] if step["kind"] == "request" and step["measure"]]
        self.assertEqual([step["request"] for step in measured], ["saved_short", "saved_long"])
        self.assertEqual(len(plan.requests), fields["derivation"]["pressure_requests_per_stage"] + 4)
        group.physical["storage_batch_pages"] = 64
        with patch.object(fixed_calibration, "_base_tokens", return_value=(list(range(16)), [16], [])):
            _, small = fixed_calibration._workload(group)
        self.assertGreater(small["derivation"]["payload_tokens"], small["derivation"]["short_tokens"])

        group.sources[0].hicache_config["page_size"] = 1
        with patch.object(fixed_calibration, "_base_tokens", return_value=([1, 2], [4096], [])):
            with self.assertRaisesRegex(ValueError, "distinct first pages"):
                fixed_calibration._workload(group)

    def test_control_acquisition_stops_before_preparation_without_budget(self):
        with (
            patch.object(control_acquisition, "capture_calibration", return_value={"status": "budget_exhausted"}),
            patch.object(control_acquisition, "prepare_group_cpu_service") as prepare,
        ):
            result = control_acquisition.prepare_control_calibration(
                SimpleNamespace(), {"definition": {"point": {"id": "stop"}}}
            )
        self.assertEqual(result, {"status": "budget_exhausted", "stage": "token_plan_capture"})
        prepare.assert_not_called()

    def test_prefetch_export_resumes_without_requiring_or_replacing_write_costs(self):
        with TemporaryDirectory(dir=ROOT_DIR) as directory:
            root = Path(directory)
            source = root / "profile.json"
            manifests = {stage: str(root / (stage + ".json")) for stage in capture.CONTROL_CAPTURE_STAGES}
            manifests["profiled_replay"] = str(source)
            attempts = [
                {"profile_manifest": manifest, "suite_config": str(root / stage / "suite.json")}
                for stage, manifest in manifests.items()
            ]
            write_json(root / "capture_ledger.json", {"attempts": attempts})
            previous = {
                "prefetch_wait": {"wait_complete": str(root / "wait.json"), "best_effort": str(root / "previous.json")},
                "write_host": {"128": {"write_through": str(root / "write.json")}},
            }
            write_json(root / "control_calibrations.json", previous)
            profile = SimpleNamespace(
                run_dir=root, hicache_config={"prefetch_policy": "best_effort", "write_policy": "write_back"}
            )
            calls = []

            def prepare_cpu(plan):
                pair = plan["pairs"][0]
                service = ROOT_DIR / pair["output_dir"] / "cpu_service.json"
                write_json(service, pair)
                return [service]

            def export(command, **kwargs):
                operation = command[command.index("--operation") + 1]
                calls.append(operation)
                self.assertEqual(operation, "prefetch_wait")
                if len(calls) == 1:
                    raise RuntimeError("interrupted export")
                output = Path(command[command.index("--output-dir") + 1]) / (operation + ".json")
                service = command[command.index("--cpu-service-cost") + 1]
                write_json(output, {"source_manifest": str(source), "cpu_service_file": str(service)})

            plan = {"definition": {"point": {"id": "stop", "page_size": 128}}, "operation": "prefetch_wait"}
            with (
                patch.object(
                    control_acquisition,
                    "capture_calibration",
                    side_effect=lambda group, definition, *, stage: {
                        "status": "already_captured",
                        "profile_manifest": manifests[stage],
                    },
                ),
                patch.object(control_acquisition, "discover_profile_runs", return_value=[profile]),
                patch.object(control_acquisition, "parse_server_command_flags", return_value={}),
                patch.object(control_acquisition, "prepare_group_cpu_service", side_effect=prepare_cpu) as prepare,
                patch.object(control_acquisition.subprocess, "run", side_effect=export),
            ):
                group = SimpleNamespace(output_dir=root)
                with self.assertRaisesRegex(RuntimeError, "interrupted"):
                    control_acquisition.prepare_control_calibration(group, plan)
                self.assertEqual(load_json(root / "control_calibrations.json"), previous)
                result = control_acquisition.prepare_control_calibration(group, plan)
                control_acquisition.prepare_control_calibration(group, plan)
                self.assertEqual(calls, ["prefetch_wait"] * 3)

                # A new light capture under the same point must not overwrite
                # the earlier CPU correction or reuse its exported costs.
                old_cost = ROOT_DIR / result["control_calibrations"]["prefetch_wait"]["best_effort"]
                old_document = load_json(old_cost)
                old_service = Path(old_document["cpu_service_file"])
                old_pair = load_json(old_service)
                manifests["light_replay"] = str(root / "new_light.json")
                ledger = load_json(root / "capture_ledger.json")
                ledger["attempts"].append(
                    {"profile_manifest": manifests["light_replay"], "suite_config": str(root / "new_light/suite.json")}
                )
                write_json(root / "capture_ledger.json", ledger)
                result = control_acquisition.prepare_control_calibration(group, plan)
                control_acquisition.prepare_control_calibration(group, plan)
                self.assertEqual(load_json(old_cost), old_document)
                self.assertEqual(load_json(old_service), old_pair)
            self.assertEqual(result["status"], "control_calibrated")
            self.assertEqual(calls, ["prefetch_wait"] * 5)
            pair = prepare.call_args.args[0]["pairs"][0]
            service = ROOT_DIR / pair["output_dir"] / "cpu_service.json"
            self.assertEqual(pair["profile_manifest"], str(source))
            self.assertEqual(pair["light_manifest"], manifests["light_replay"])
            self.assertEqual(pair["tp_size"], 1)
            merged = load_json(root / "control_calibrations.json")
            self.assertEqual(merged["write_host"], controls.control_calibrations(previous)["write_host"])
            self.assertNotIn("write_host", result["control_calibrations"])
            self.assertEqual(set(merged["prefetch_wait"]), {"best_effort", "wait_complete"})
            self.assertEqual(
                merged["prefetch_wait"]["best_effort"], result["control_calibrations"]["prefetch_wait"]["best_effort"]
            )
            self.assertEqual(
                load_json(root / "capture_ledger.json")["attempts"][1]["cpu_service_cost"],
                str(repo_relative_path(service)),
            )

    def test_stage_artifacts_require_matching_measurement_mode_and_verified_replay(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            summary = dict(
                status="completed",
                dry_run=False,
                profile_manifests=[str(root / "run/profile_manifest.json")],
                generated_forced_token_bundle={"path": str(root / "bundle.json")},
            )
            write_json(root / "bundle.json", {})
            write_json(root / "suite_result.json", summary)
            write_json(root / "unrelated/profile_manifest.json", {"status": "failed"})
            report = dict(
                status="completed",
                requests=[dict(kind="request", origin_input_count=8, actual_output_count=2)],
                forced_token=dict(
                    enabled=True,
                    mode="replay",
                    request_count=1,
                    output_checked_count=1,
                    mismatch_count=0,
                    unchecked_count=0,
                    prompt_mismatch_count=0,
                    all_actual_outputs_match_plan=True,
                ),
            )
            for stage, torch, valid_tokens, collection_errors, accepted in [
                ("token_plan_capture", 0, True, [], True),
                ("light_replay", 0, True, [], True),
                ("light_replay", 1, True, [], False),
                ("profiled_replay", 0, True, [], False),
                ("light_replay", 0, False, [], False),
                ("light_replay", 0, True, ["hook collection failed"], False),
            ]:
                with self.subTest(stage=stage, torch=torch, tokens=valid_tokens):
                    write_json(
                        root / "run/profile_manifest.json",
                        dict(
                            status="completed",
                            collection_errors=collection_errors,
                            profiling={"enabled": stage != "token_plan_capture"},
                            bench={
                                "workload_report_files": [{"path": str(root / "run/bench/work/workload_report.json")}]
                            },
                            trace_channel_coverage=dict(
                                ld_preload_trace_files=int(stage != "token_plan_capture"),
                                torch_trace_files=torch,
                                python_probe_trace_files=0,
                            ),
                        ),
                    )
                    report["forced_token"]["all_actual_outputs_match_plan"] = valid_tokens
                    write_json(root / "run/bench/work/workload_report.json", report)
                    row = dict(
                        suite_dir=str(root),
                        stage=stage,
                        mode="capture" if stage == "token_plan_capture" else "replay",
                        status="completed",
                        budgeted_output_tokens_per_request=2,
                    )
                    with (
                        patch.object(capture, "repo_relative_path", side_effect=Path),
                        patch.object(capture, "run_container_attempt"),
                    ):
                        capture.run_profile_attempt(row, [], 60)
                    self.assertEqual(row["status"] == "completed", accepted)
                    self.assertEqual(row["charged_tokens"], 10)
            # An old manifest in another directory must not substitute for a
            # missing declared output, including when accounting a failed run.
            summary["profile_manifests"] = [str(root / "missing/profile_manifest.json")]
            write_json(root / "suite_result.json", summary)
            for status in ("completed", "failed"):
                row = dict(suite_dir=str(root), mode="replay", status=status, reserved_requests=1, reserved_tokens=10)
                capture._capture_artifacts(row)
                self.assertEqual(row["status"], "invalid_capture_output" if status == "completed" else "failed")
                row["calibration_inputs"] = {"template": str(root / "absent.json"), "config_specs": "absent.json"}
                self.assertEqual(capture.capture_usage([row])["tokens"], 10)
                self.assertNotIn("profile_manifest", row)

            # An interrupted write is secondary evidence, not a new root cause.
            (root / "suite_result.json").write_text("{", encoding="utf-8")
            for status in ("completed", "failed", "budget_timeout", "interrupted", "cleanup_incomplete"):
                row = dict(suite_dir=str(root), mode="replay", status=status, reserved_requests=1, reserved_tokens=10)
                with self.subTest(status=status), patch.object(capture, "run_container_attempt"):
                    capture.run_profile_attempt(row, [], 60)

                self.assertEqual(row["status"], "invalid_capture_output" if status == "completed" else status)
                self.assertIn("artifact_error", row)
                self.assertEqual(capture.capture_usage([row])["tokens"], 10)

    def test_existing_base_needs_no_matrix_suite(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            config, request, target_path = root / "base.json", root / "group.json", root / "target.json"
            write_json(
                config,
                dict(
                    framework="sglang",
                    profiling={"enabled": True},
                    run_root="old_profiles",
                    env={"SGLANG_STEP_TIMING_DIR": "old_timing"},
                    server={"command": ["python3", "server.py"], "ready_url": "http://localhost/health"},
                    bench={},
                ),
            )
            source = SimpleNamespace(
                config_id="base",
                input_id="work",
                config_path=config,
                hicache_config={"page_size": 128},
                manifest_path=root / "profile.json",
            )
            target = dict(name="target", hicache={"page_size": 64})
            write_json(target_path, target)
            raw = dict(
                base_manifests=[str(source.manifest_path)], target_configs=[target], output_dir=str(root / "output")
            )
            with (
                patch.object(group_module, "discover_profile_runs", return_value=[source]),
                patch.object(group_module, "source_environment", return_value={}),
                patch.object(group_module, "captured_physical_declaration", return_value=None),
            ):
                for declaration in (target, str(target_path)):
                    write_json(request, {**raw, "target_configs": [declaration]})
                    group = group_module.GroupRequest.load(request)
                    self.assertIsNone(group.profile_suite)
                    self.assertEqual((group.base_config, group.workload_ids), ("base", ("work",)))
                    self.assertEqual(group.budget.server_starts, 0)
                    self.assertIsNone(group.token_plan("work"))

                source.hicache_config = None
                with self.assertRaisesRegex(ValueError, "use build-dag"):
                    group_module.GroupRequest.load(request)
                source.hicache_config = {"page_size": 128}

                # An admitted replay supplies its own token source without a suite or ledger.
                token_plan = root / "tokens.json"
                write_json(token_plan, {"workload_id": "work", "requests": []})
                replay_config = load_json(config)
                replay_config["bench"]["command"] = [
                    "python3",
                    "hicache_template_workload.py",
                    "--template=unused.json",
                    "--output-dir=unused",
                    "--forced-token-mode=replay",
                    f"--forced-token-plan={token_plan}",
                ]
                write_json(config, replay_config)
                self.assertEqual(group.token_plan("work"), token_plan)
                write_json(token_plan, {"workload_id": "other", "requests": []})
                with self.assertRaisesRegex(ValueError, "another workload"):
                    group.token_plan("work")

                for changes in (
                    {"workload_ids": ["missing"]},
                    {"target_configs": [target, target]},
                    {"target_configs": [{"name": "bad", "hicache": {"page_size": 64, "io_cost": {}}}]},
                ):
                    write_json(request, {**raw, **changes})
                    with self.assertRaises(ValueError):
                        group_module.GroupRequest.load(request)
            with (
                patch.object(fixed_calibration, "_base_tokens", return_value=([1, 2], [4096], [])),
                patch.object(fixed_calibration, "repo_relative_path", side_effect=Path),
            ):
                definition = fixed_calibration.materialize_fixed_calibration(group, phase_only=True)
            generated = load_json(Path(definition["point"]["files"]["capture_suite"]))
            self.assertEqual(generated["env"]["SGLANG_STEP_TIMING_DIR"], "{run_dir}/timing")
            self.assertEqual(generated["run_root"], str(group.output_dir / "calibration_profiles"))
            (experiment,) = expand_suite(generated)
            self.assertEqual(experiment["metadata"]["workload_id"], definition["name"])
            self.assertEqual(experiment["metadata"]["config_id"], definition["point"]["id"])
            self.assertEqual(experiment["bench"], generated["bench"])

    def test_tokens_come_from_selected_base_config_without_group_bundle(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            plan, config = root / "tokens.json", root / "config.json"
            write_json(root / "profile_manifest.json", {"bench": {"workload_report_files": []}})
            write_json(
                config,
                {
                    "bench": {
                        "command": [
                            "hicache_template_workload.py",
                            "--template",
                            "unused.json",
                            "--output-dir",
                            str(root),
                            "--forced-token-mode=replay",
                            "--forced-token-plan=" + str(plan),
                        ]
                    }
                },
            )
            group = SimpleNamespace(
                workload_ids=["base_work"],
                raw={},
                token_plan=lambda workload: group_module.GroupRequest.token_plan(group, workload),
                sources=[
                    SimpleNamespace(
                        input_id="base_work", config_path=config, manifest_path=root / "profile_manifest.json"
                    )
                ],
            )
            for workload, tokens, valid in [
                ("base_work", [1, 2, 3], True),
                ("target_work", [1, 2, 3], False),
                ("base_work", [1, True, 3], False),
            ]:
                write_json(plan, {"workload_id": workload, "requests": [{"origin_input_ids": tokens}]})
                with patch.object(fixed_calibration, "repo_relative_path", side_effect=Path):
                    if valid:
                        self.assertEqual(fixed_calibration._base_tokens(group), ([1, 2, 3], [3], [str(plan)]))
                    else:
                        with self.assertRaises(ValueError):
                            fixed_calibration._base_tokens(group)

    def test_normal_base_report_supplies_tokens_without_template_or_replay(self):
        from ..workload_template.expand import CanonicalPlan, RequestPlan
        from ..workload_template.executor import execute_workload

        with TemporaryDirectory() as directory:
            root = Path(directory)
            request = RequestPlan("step", 0, "base_work:step", "prompt", "compute", True, (1, 2, 3), 1, 2, 2)
            plan = CanonicalPlan(SimpleNamespace(workload_id="base_work"), (request,), "step", "step")
            with patch(
                "markov_internal.workload_template.executor._post_json",
                return_value={"status": "ok", "start_time_ms": 1.0, "end_time_ms": 2.0, "latency_ms": 1.0},
            ):
                execute_workload(
                    plan,
                    base_url="http://unused",
                    output_dir=root / "bench",
                    mode="none",
                    forced_token_plan_path=None,
                    config=None,
                    timeout_sec=1,
                    diagnostic_api_key=None,
                    require_diagnostic=False,
                )
            report_path = root / "bench/workload_report.json"
            report = load_json(report_path)
            self.assertEqual(report["input_tokens"], {"prompt": [1, 2, 3]})
            self.assertFalse(report["forced_token"]["enabled"])
            manifest = root / "profile_manifest.json"
            write_json(manifest, {"bench": {"workload_report_files": [{"path": str(report_path)}]}})
            group = SimpleNamespace(
                workload_ids=["base_work"],
                token_plan=lambda _: None,
                sources=[
                    SimpleNamespace(
                        input_id="base_work",
                        manifest_path=manifest,
                        config_path=root / "absent.json",
                        workload_window=discover_workload_window({}, manifest),
                    )
                ],
            )
            with patch.object(fixed_calibration, "repo_relative_path", side_effect=Path):
                self.assertEqual(fixed_calibration._base_tokens(group), ([1, 2, 3], [3], [str(report_path)]))
                for field, value in [
                    ("workload_id", "target_work"),
                    ("status", "failed"),
                    ("input_tokens", {"prompt": [1, True, 3]}),
                ]:
                    write_json(report_path, {**report, field: value})
                    with self.assertRaises(ValueError):
                        fixed_calibration._base_tokens(group)

    def test_completed_capture_is_reused_and_control_stages_resume(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            suite = root / "suite.json"
            # Later suite edits must not replace the admitted base's captured environment.
            write_json(suite, {"server": {"command": ["changed-server"]}})
            base_config = root / "base.json"
            write_json(
                base_config,
                {
                    "run_root": str(root / "runs"),
                    "profiling": {"enabled": True},
                    "env": {"BASE_RESOURCE_SETTING": "retained"},
                    "server": {
                        "command": [
                            "python3",
                            "server.py",
                            "--page-size=64",
                            "--page-size=256",
                            '--hicache-storage-backend-extra-config={"storage_setting":"retained"}',
                        ],
                        "ready_url": "http://localhost/health",
                    },
                },
            )
            group = SimpleNamespace(
                output_dir=root,
                profile_suite=suite,
                base_config="base",
                sources=[SimpleNamespace(config_path=base_config, hicache_config={"page_size": 128})],
                physical=dict(
                    kv_geometry={"kv_bytes_per_token_per_rank": 1},
                    service_models={"load": {"page_bandwidth_points": [{"page_bytes": 128}]}},
                    storage_batch_pages=64,
                ),
                workload_ids=["base"],
                budget=group_module.ProfileBudget(wall_seconds=100.0, server_starts=2, requests=100, tokens=1000000),
            )
            with (
                patch.object(fixed_calibration, "repo_relative_path", side_effect=Path),
                patch.object(fixed_calibration, "_base_tokens", return_value=(list(range(16)), [4096], [])),
            ):
                for phase_only, release_only in ((True, False), (False, True), (False, False)):
                    definition = fixed_calibration.materialize_fixed_calibration(
                        group, phase_only=phase_only, release_only=release_only
                    )
                    compiled = fixed_calibration.expand_template(
                        fixed_calibration.load_template(Path(definition["files"]["template"])), None
                    )
                    self.assertEqual(
                        definition["counts"],
                        dict(
                            requests=len(compiled.requests),
                            tokens=sum(len(row.prompt_token_ids) + row.max_new_tokens for row in compiled.requests),
                            output_tokens_per_request=1 if release_only else 2,
                        ),
                    )
                    if release_only:
                        template = load_json(Path(definition["files"]["template"]))
                        self.assertEqual(template["steps"][2]["assertions"][0]["expect"]["host"], "none")
                        self.assertTrue(all(step["kind"] == "request" for step in template["steps"][3:]))
                        self.assertEqual(
                            len({tuple(row.prompt_token_ids) for row in compiled.requests}), len(compiled.requests)
                        )
                        self.assertLessEqual(len(compiled.requests), 5)
                        spec = load_json(Path(definition["files"]["config_specs"]))["configs"][0]["policy"]
                        self.assertEqual(spec["write_policy"], "write_through_selective")
            point = definition["point"]
            self.assertEqual(set(point["files"]), {"capture_suite"})
            generated = load_json(Path(point["files"]["capture_suite"]))
            self.assertTrue(generated["profiling"]["enabled"])
            self.assertEqual(generated["env"], {"BASE_RESOURCE_SETTING": "retained"})
            flags = fixed_calibration.parse_server_command_tokens(generated["server"]["command"])
            self.assertEqual(flags["page_size"], "128")
            self.assertEqual(json.loads(flags["hicache_storage_backend_extra_config"])["storage_setting"], "retained")
            self.assertFalse(list((root / "generated").rglob("*_replay.json")))
            with (
                patch.object(fixed_calibration, "repo_relative_path", side_effect=Path),
                patch.object(fixed_calibration, "_base_tokens", return_value=(list(range(16)), [4096], [])),
            ):
                stop = fixed_calibration.materialize_fixed_calibration(group, prefetch_policy="best_effort")
            with patch.object(fixed_calibration, "materialize_fixed_calibration", return_value=definition):
                primitive = fixed_calibration.plan_fixed_calibration(
                    group, {"missing": [{"component": "control/load", "reason": "cpu_requires_paired_service"}]}
                )
            self.assertEqual(primitive["status"], "no_suitable_experiment")
            self.assertNotEqual(stop["files"]["template"], definition["files"]["template"])
            spec = load_json(Path(stop["files"]["config_specs"]))["configs"][0]["policy"]
            self.assertEqual(spec["prefetch_stop_policy"], "best_effort")
            command = load_json(Path(stop["point"]["files"]["capture_suite"]))["server"]["command"]
            self.assertEqual(command[command.index("--hicache-storage-prefetch-policy") + 1], "best_effort")
            prior = dict(
                status="completed",
                mode="capture",
                profile_captured=True,
                calibration_point=point["id"],
                profile_manifest="prior-profile",
                calibration_inputs=definition["files"],
                forced_token_bundle="absent-old-bundle",
                reserved_requests=1,
                reserved_tokens=1,
                wall_seconds=1,
            )
            write_json(root / "capture_ledger.json", {"attempts": [prior]})

            with (
                patch.object(capture, "require_repo_path", side_effect=Path),
                patch.object(capture, "repo_relative_path", side_effect=Path),
                patch.object(capture, "run_profile_attempt") as run,
            ):
                result = capture.capture_calibration(group, definition)
            self.assertEqual(result["status"], "already_captured")
            self.assertEqual(result["profile_manifest"], "prior-profile")
            self.assertEqual(result["usage"]["server_starts"], 1)
            run.assert_not_called()
            group.budget = replace(group.budget, server_starts=6)
            failed = False

            def staged(row, command, remaining):
                nonlocal failed
                stage = row["stage"]
                config = load_json(Path(row["suite_config"]))
                self.assertEqual(config["metadata"]["profile_mode"], "forced_token_" + row["mode"])
                if stage == "token_plan_capture":
                    self.assertEqual(config["profiling"], {"enabled": False, "channels": []})
                    bundle = root / "bundle.json"
                    write_json(bundle, {})
                    row["forced_token_bundle"] = str(bundle)
                else:
                    self.assertIn("--forced-token-bundle", command)
                    config = load_json(Path(row["suite_config"]))
                    argv = config["bench"]["command"]
                    self.assertEqual(argv[argv.index("--forced-token-mode") + 1], "replay")
                    if stage == "light_replay":
                        self.assertEqual(config["profiling"]["channels"], ["ld_preload"])
                        self.assertEqual(row["forced_token_bundle"], str(root / "bundle.json"))
                    else:
                        self.assertTrue(config["profiling"]["enabled"])
                row.update(
                    status="completed",
                    profile_captured=stage == "profiled_replay",
                    profile_manifest=stage,
                    wall_seconds=1,
                )
                if stage == "profiled_replay" and not failed:
                    failed = True
                    row["status"] = "failed"

            with (
                patch.object(capture, "require_repo_path", side_effect=Path),
                patch.object(capture, "repo_relative_path", side_effect=Path),
                patch.object(capture, "run_profile_attempt", side_effect=staged) as run,
            ):

                def acquire(stage):
                    return capture.capture_calibration(group, definition, stage=stage)

                self.assertEqual(acquire("light_replay")["status"], "missing_capture_stage")
                for stage in ("token_plan_capture", "profiled_replay", "profiled_replay"):
                    acquire(stage)
                ledger = load_json(root / "capture_ledger.json")
                seed = next(row for row in ledger["attempts"] if row.get("stage") == "token_plan_capture")
                write_json(root / "later_bundle.json", {"different": "token outputs"})
                ledger["attempts"].append(dict(seed, forced_token_bundle=str(root / "later_bundle.json")))
                write_json(root / "capture_ledger.json", ledger)
                acquire("light_replay")
                ledger = load_json(root / "capture_ledger.json")
                ledger["attempts"].append(
                    dict(
                        ledger["attempts"][-1],
                        forced_token_bundle=str(root / "later_bundle.json"),
                        profile_manifest="unpaired_light",
                    )
                )
                write_json(root / "capture_ledger.json", ledger)
                reused = acquire("light_replay")
                self.assertEqual(reused["status"], "already_captured")
                self.assertEqual(reused["profile_manifest"], "light_replay")
                self.assertEqual(run.call_count, 4)
                blocked = capture.capture_calibration(group, stop, stage="token_plan_capture")
                self.assertEqual(blocked["status"], "budget_exhausted")
                self.assertEqual(blocked["usage"]["server_starts"], 7)
            attempts = load_json(root / "capture_ledger.json")["attempts"]
            self.assertFalse(capture.completed_profile(attempts[-1]))
            self.assertFalse(capture.completed_profile(attempts[1]))
            full = next(
                row for row in attempts if row.get("stage") == "profiled_replay" and row["status"] == "completed"
            )
            self.assertTrue(capture.completed_profile(full))
            self.assertEqual(attempts[-1]["calibration_inputs"], full["calibration_inputs"])
            self.assertEqual(attempts[-1]["calibration_inputs"], attempts[1]["calibration_inputs"])

    def test_changed_work_uses_remaining_budget_not_old_point_result(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            files = {}
            for label in ("old", "new"):
                files[label] = {key: str(root / (label + "_" + key + ".json")) for key in ("template", "config_specs")}
                for path in files[label].values():
                    write_json(Path(path), {"experiment": label})
            attempt = dict(
                status="completed",
                mode="capture",
                profile_captured=True,
                calibration_point="point",
                profile_manifest="old-profile",
                calibration_inputs=files["old"],
                reserved_requests=3,
                reserved_tokens=30,
                wall_seconds=10.0,
            )
            write_json(root / "capture_ledger.json", dict(base_config="base", attempts=[attempt]))
            group = SimpleNamespace(
                output_dir=root,
                base_config="base",
                fixed_calibration_manifests=[],
                budget=group_module.ProfileBudget(wall_seconds=100.0, server_starts=1, requests=10, tokens=100),
            )
            for label, expected in [("old", "already_captured"), ("new", "budget_exhausted")]:
                definition = dict(
                    files=files[label],
                    point=dict(id="point", page_size=128),
                    counts=dict(requests=2, tokens=20, output_tokens_per_request=2),
                )
                with (
                    self.subTest(label=label),
                    patch.object(capture, "require_repo_path", side_effect=Path),
                    patch.object(capture, "start_process") as launch,
                ):
                    result = capture.capture_calibration(group, definition)
                self.assertEqual(result["status"], expected)
                self.assertEqual(result["usage"]["server_starts"], 1)
                launch.assert_not_called()
                with (
                    patch.object(capture, "require_repo_path", side_effect=Path),
                    patch.object(fixed_calibration, "materialize_fixed_calibration", return_value=definition),
                ):
                    plan = fixed_calibration.plan_fixed_calibration(group, {"missing": [{"component": "phase"}]})
                self.assertEqual(plan["completed_profile_count"], 1 if label == "old" else 0)
                self.assertEqual(plan["usage"]["server_starts"], 1)
                self.assertNotEqual(plan["status"], "input_conflict")
                self.assertEqual(plan["status"], "experiment_exhausted" if label == "old" else "needs_calibration")

    def test_phase_gap_selects_small_shared_compute_workload(self):
        group = SimpleNamespace(sources=[SimpleNamespace(hicache_config={"page_size": 32})])
        with patch.object(fixed_calibration, "_base_tokens", return_value=([1, 2], [4096], [])):
            template, fields = fixed_calibration._phase_workload(group)
        self.assertEqual(fields["point"]["page_size"], 32)
        self.assertEqual({step["phase"] for step in template["steps"]}, {"compute"})
        point = fields["point"]
        from ..workload_template.schema import load_template
        from ..workload_template.expand import expand_template
        from .phase_calibration import prefill_features, decode_features, _feature_rank

        with TemporaryDirectory() as directory:
            path = Path(directory) / "workload.json"
            write_json(path, template)
            compiled = expand_template(load_template(path), None)
            self.assertGreater(
                point["device_tokens"], sum(len(row.prompt_token_ids) + row.max_new_tokens for row in compiled.requests)
            )
            config = Path(directory) / "config.json"
            write_json(Path(directory) / "profile_manifest.json", {"bench": {"workload_report_files": []}})
            write_json(
                config,
                {
                    "bench": {
                        "command": [
                            "hicache_template_workload.py",
                            "--template=" + str(path),
                            "--output-dir",
                            directory,
                        ]
                    }
                },
            )
            base = SimpleNamespace(
                workload_ids=["base_work"],
                token_plan=lambda _: None,
                sources=[
                    SimpleNamespace(
                        input_id="base_work",
                        config_path=config,
                        manifest_path=Path(directory) / "profile_manifest.json",
                        workload_window=None,
                    )
                ],
            )
            with patch.object(fixed_calibration, "repo_relative_path", side_effect=Path):
                _, lengths, sources = fixed_calibration._base_tokens(base)
            self.assertEqual(len(lengths), 4)
            self.assertEqual(sources, [str(path)])
        self.assertEqual(len(compiled.requests), 4)
        self.assertTrue(all(request.max_new_tokens == 2 for request in compiled.requests))
        prefix = template["request_defs"]["seed"]["prompt_token_ids"]
        prefill, decode = [], []
        for name, definition in template["request_defs"].items():
            if name == "seed":
                continue
            values = definition["prompt_token_ids"]
            context = 0 if name == "seed" else len(prefix)
            self.assertEqual(values[: len(prefix)], prefix)
            prefill.append(dict(zip(("fixed", "new", "attention"), prefill_features(len(values) - context, context))))
            decode.append(dict(zip(("fixed", "context", "pages"), decode_features(len(values), 32, 32))))
        self.assertEqual(_feature_rank(prefill, ("fixed", "new", "attention")), 3)
        self.assertEqual(_feature_rank(decode, ("fixed", "context", "pages")), 3)
        with TemporaryDirectory() as directory:
            group.output_dir = Path(directory)
            definition = dict(files={}, **fields)
            with (
                patch.object(fixed_calibration, "materialize_fixed_calibration", return_value=definition) as build,
                patch.object(fixed_calibration, "calibration_inputs", return_value={}),
            ):
                plan = fixed_calibration.plan_fixed_calibration(group, {"missing": [{"component": "phase"}]})
            build.assert_called_once_with(group, phase_only=True)
            self.assertEqual(plan["experiment_scope"], "phase")
            self.assertEqual(plan["remaining_profile_count"], 1)
        with patch.object(fixed_calibration, "materialize_fixed_calibration") as build:
            for component in ("base", "service/load", "physical/prefetch_stages"):
                result = fixed_calibration.plan_fixed_calibration(group, {"missing": [{"component": component}]})
                self.assertEqual(result["status"], "no_suitable_experiment")
            build.assert_not_called()
