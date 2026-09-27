"""A config check must never overwrite a previous capture."""

import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

from .executor import ProfileRun
from .environments import PYTHON_PROBE_ROOT, build_bench_env, build_server_env
from .runtime import model_path_from_config, temporary_model_config
from . import runner
from ..common.io import load_json, write_json


class ExistingRunTests(unittest.TestCase):
    def test_single_replay_uses_explicit_plan_and_checks_it_before_run(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            template, plan = root / "template.json", root / "plan.json"
            write_json(template, {"id": "workload"})
            token_plan = {
                "workload_id": "workload",
                "requests": [{"logical_request_id": "r", "origin_input_ids": [1], "forced_output_ids": [2]}],
            }
            write_json(plan, token_plan)
            cfg = {
                "run_root": str(root),
                "run_id": "replay",
                "server": {"command": ["server"]},
                "profiling": {"enabled": False},
                "bench": {
                    "command": [
                        "python3",
                        "scripts/bench/hicache_template_workload.py",
                        "--template",
                        str(template),
                        "--output-dir",
                        "{bench_dir}",
                        "--forced-token-mode",
                        "replay",
                        "--forced-token-plan",
                        str(plan),
                    ]
                },
            }
            paths = runner.run_profile_suite(cfg, True, [(1, cfg)])
            self.assertEqual(load_json(paths[0] / "profile_manifest.json")["status"], "dry_run")

            token_plan["workload_id"] = "other"
            write_json(plan, token_plan)
            with self.assertRaisesRegex(ValueError, "forced token preflight"):
                ProfileRun(cfg, dry_run=False)

    def test_prepared_probe_selection_controls_server_bench_and_manifest(self):
        for enabled in (True, False):
            with self.subTest(enabled=enabled), tempfile.TemporaryDirectory() as temporary:
                cfg = {
                    "run_root": temporary,
                    "run_id": "capture",
                    "post_workload_drain_sec": 1,
                    "server": {"command": ["server", "--model-path", "unused", "--model_path=namespace/model"]},
                    "profiling": {
                        "enabled": enabled,
                        "channels": ["python_probe"],
                        "python_probe": {"consumers": ["hicache_dag_patch"], "flush_interval_sec": 0.5},
                    },
                    "env": {
                        "TRACE_SIM_PYTHON_PROBE": "1",
                        "TRACE_SIM_PYTHON_PROBE_DEBUG": "1",
                        "TRACE_SIM_PROFILE_CONFIG_ID": "old_config",
                        "TRACE_SIM_PROFILE_INPUT_ID": "old_workload",
                        "TRACE_SIM_PROFILE_MODEL_PATH": "old_model",
                        "PYTHONPATH": f"{PYTHON_PROBE_ROOT}:/unrelated",
                    },
                }
                with patch("markov_internal.profiling.executor.select_python_probe_targets") as select:
                    select.return_value = [{"id": "prepared_target"}]
                    run = ProfileRun(cfg, dry_run=True)
                    select.side_effect = AssertionError("must not reselect during execution or finalization")
                    env = build_server_env(cfg, run.runtime, run.layout, run.adapter, run.python_targets)
                    bench = build_bench_env(cfg, env, run.layout, run.model_path)
                    run.run()

                manifest = load_json(run.layout.run_dir / "profile_manifest.json")
                contract = manifest["profiling"]["python_target_contract"]
                tail = manifest["profiling"]["capture_tail_contract"]
                self.assertEqual(tail["post_workload_drain_sec"], run.runtime.post_workload_drain_sec)
                self.assertEqual(tail["python_probe_flush_interval_sec"], 0.5)
                self.assertNotIn("TRACE_SIM_PYTHON_PROBE_DEBUG", env)
                self.assertNotIn("TRACE_SIM_PYTHON_PROBE", bench)
                self.assertNotIn("TRACE_SIM_PYTHON_PROBE_TARGETS", bench)
                self.assertEqual(bench["PYTHONPATH"], "/unrelated")
                self.assertEqual(run.model_path, "namespace/model")
                self.assertEqual(bench["TRACE_SIM_PROFILE_MODEL_PATH"], run.model_path)
                self.assertNotIn("TRACE_SIM_PROFILE_CONFIG_ID", bench)
                self.assertNotIn("TRACE_SIM_PROFILE_INPUT_ID", bench)
                if enabled:
                    self.assertEqual(env["TRACE_SIM_PYTHON_PROBE"], "1")
                    self.assertEqual(
                        float(env["TRACE_SIM_PYTHON_PROBE_FLUSH_INTERVAL_SEC"]), tail["python_probe_flush_interval_sec"]
                    )
                    injected = json.loads(env["TRACE_SIM_PYTHON_PROBE_TARGETS"])
                    self.assertEqual(contract["selected_target_ids"], [target["id"] for target in injected])
                    self.assertEqual(contract["selected_target_count"], len(injected))
                else:
                    self.assertNotIn("TRACE_SIM_PYTHON_PROBE", env)
                    self.assertEqual(env["PYTHONPATH"], "/unrelated")
                    self.assertIsNone(contract)

    def test_suite_failure_is_reported_after_allowed_remaining_runs(self):
        for failure, keep_going in (
            (None, True),
            (RuntimeError("failed"), True),
            (RuntimeError("failed"), False),
            (KeyboardInterrupt(), True),
        ):
            with self.subTest(failure=failure, keep_going=keep_going), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                cfg = {
                    "experiments": [{"name": "first"}, {"name": "second"}],
                    "run_root": str(root),
                    "run_id": "suite",
                    "continue_on_error": keep_going,
                }
                attempted = []

                def prepare(config, *, dry_run):
                    run_dir = Path(config["run_root"]) / config["run_id"]

                    def execute():
                        declared = load_json(root / "suite/suite_result.json")
                        self.assertEqual(declared["status"], "running")
                        self.assertIn(str(run_dir / "profile_manifest.json"), declared["profile_manifests"])
                        attempted.append(config["name"])
                        if config["name"] == "first" and failure is not None:
                            raise failure
                        return run_dir

                    return SimpleNamespace(
                        cfg=config,
                        run=execute,
                        layout=SimpleNamespace(run_dir=run_dir),
                    )

                with patch.object(runner, "ProfileRun", side_effect=prepare):
                    selected = list(enumerate(runner.expand_suite(cfg), start=1))
                    if failure is None:
                        self.assertEqual(len(runner.run_profile_suite(cfg, False, selected)), 2)
                    else:
                        with self.assertRaises(type(failure)):
                            runner.run_profile_suite(cfg, False, selected)
                result = load_json(root / "suite/suite_result.json")
                expected_attempts = 2 if failure is None or keep_going and isinstance(failure, Exception) else 1
                self.assertEqual(len(attempted), expected_attempts)
                self.assertEqual(result["attempted_count"], expected_attempts)
                self.assertEqual(result["status"], "completed" if failure is None else "failed")
                self.assertEqual(result["aborted_count"], 2 - expected_attempts)
                self.assertEqual(len(result["profile_manifests"]), 2)

    def test_model_config_restores_bytes_after_partial_write_or_interrupt(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            config = root / "config.json"
            original = b'{ "setting": 1 }\n'
            config.write_bytes(original)
            cfg = {"model_path": str(root), "model_config_overrides": {"setting": 2}}
            layout = SimpleNamespace(run_dir=root / "run")
            model_path = model_path_from_config(cfg, ["server", "--model-path", "ignored"])
            with self.assertRaises(KeyboardInterrupt):
                with temporary_model_config(cfg, model_path, layout):
                    self.assertNotEqual(config.read_bytes(), original)
                    raise KeyboardInterrupt()
            self.assertEqual(config.read_bytes(), original)

            def partial_write(path, data):
                path.write_bytes(b"{")
                raise OSError("partial write")

            with patch("markov_internal.profiling.runtime.write_json", side_effect=partial_write):
                with self.assertRaises(OSError):
                    with temporary_model_config(cfg, model_path, layout):
                        self.fail("capture must not start after a failed override")
            self.assertEqual(config.read_bytes(), original)

    def test_startup_interrupt_stops_server_without_retry(self):
        run = ProfileRun.__new__(ProfileRun)
        run.server_cfg = {"startup_max_attempts": 3}
        run.server_command = ["server"]
        run.framework = "sglang"
        run.adapter = SimpleNamespace(default_ready_url="http://localhost/ready")
        run.layout = SimpleNamespace(log_dir=Path("unused"))
        module = "markov_internal.profiling.executor."
        with (
            patch(module + "start_process") as start,
            patch(module + "stop_process") as stop,
            patch(module + "wait_for_ready", side_effect=KeyboardInterrupt),
        ):
            with self.assertRaises(KeyboardInterrupt):
                run._start_server({})
            self.assertEqual(start.call_count, 1)
            stop.assert_called_once_with(start.return_value)

    def test_workload_process_is_released_on_return_failure_or_interrupt(self):
        run = ProfileRun.__new__(ProfileRun)
        run.cfg, run.server_cfg = {}, {}
        run.server_command, run.bench_command = ["server"], ["bench"]
        run.model_path = None
        run.layout = SimpleNamespace(log_dir=Path("unused"))
        module = "markov_internal.profiling.executor."
        for outcome in (0, 1, KeyboardInterrupt()):
            with (
                self.subTest(outcome=outcome),
                patch(module + "build_bench_env", return_value={}),
                patch(module + "start_process") as start,
                patch(module + "stop_process") as stop,
            ):
                start.return_value.wait.side_effect = [outcome]
                if outcome == 0:
                    run._run_bench({})
                else:
                    with self.assertRaises(
                        KeyboardInterrupt if isinstance(outcome, KeyboardInterrupt) else RuntimeError
                    ):
                        run._run_bench({})
                stop.assert_called_once_with(start.return_value)

    def test_nonempty_run_is_untouched(self):
        for dry_run, clean in ((True, False), (True, True), (False, False)):
            for filename in ("profile_manifest.json", "raw_trace.json"):
                with (
                    self.subTest(dry_run=dry_run, clean=clean, filename=filename),
                    tempfile.TemporaryDirectory() as temporary,
                ):
                    directory = Path(temporary)
                    original = directory / filename
                    original.write_text("previous capture\n")
                    run = ProfileRun.__new__(ProfileRun)
                    run.cfg = {"clean_run_dir": clean}
                    run.dry_run = dry_run
                    run.layout = SimpleNamespace(run_dir=directory, prepare=Mock())
                    with self.assertRaises(FileExistsError):
                        run.run()
                    run.layout.prepare.assert_not_called()
                    self.assertEqual(original.read_text(), "previous capture\n")
                    self.assertEqual(list(directory.iterdir()), [original])


if __name__ == "__main__":
    unittest.main()
