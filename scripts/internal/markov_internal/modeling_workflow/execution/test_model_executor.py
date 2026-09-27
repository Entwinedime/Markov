"""Execution keeps bounded failure evidence without duplicate in-memory rows."""

from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace as NS
import unittest
from unittest.mock import patch

from ...common.paths import ROOT_DIR
from ...modeling.run_config import ModelingRunConfig
from ..context import DiagnosticLevel
from ..types import ModelRunSpec, TargetHiCacheConfig
from . import model_executor as module


class ModelExecutorTests(unittest.TestCase):
    def spec(self, directory):
        root = Path(directory)
        source = NS(
            manifest_path=root / "profile_manifest.json",
            hicache_config={"page_size": 64, "prefetch_policy": "timeout"},
            workload_window=None,
        )
        return ModelRunSpec("cell", root, source, TargetHiCacheConfig("target", source.hicache_config))

    def test_only_new_failure_evidence_is_reported(self):
        with TemporaryDirectory(dir=ROOT_DIR) as temporary:
            options = NS(
                dry_run=False,
                diagnostics=DiagnosticLevel("off"),
                trace_threads=1,
                trace_file_threads=1,
                hicache_io_model=NS(narrow_config=lambda *args: {}),
            )
            spec = self.spec(temporary)
            report = spec.output_dir / "run_summary.json"
            need = {"component": "execution_control/eviction_locked_candidate", "coordinates": {"heap_size": 8}}
            with (
                patch.object(module, "run_command") as command,
                patch("markov_internal.modeling.backend.trace_graph_executable", return_value=Path("trace_graph")),
            ):

                def execute(*args, **kwargs):
                    module.write_json(report, {"status": "data_limitation", "missing_costs": [need]})
                    return NS(returncode=1)

                command.side_effect = execute
                self.assertEqual(module._run_one(options, spec).missing_costs, (need,))
                command.side_effect = lambda *args, **kwargs: NS(returncode=1)
                self.assertEqual(module._run_one(options, spec).missing_costs, ())
                self.assertFalse(report.exists())

                def broken_report(*args, log_path):
                    log_path.write_text("original model failure\n", encoding="utf-8")
                    report.write_text("{", encoding="utf-8")
                    return NS(returncode=1)

                command.side_effect = broken_report
                result = module._run_one(options, spec)
                self.assertEqual(result.return_code, 1)
                log = (spec.output_dir / "model.log").read_text(encoding="utf-8")
                self.assertIn("original model failure", log)
                self.assertIn("internal model runner error", log)
                options.dry_run = True
                self.assertEqual(module._run_one(options, spec).missing_costs, ())

    def test_parallel_failure_stops_replacement_for_the_whole_completed_batch(self):
        options = NS(model_run_jobs=2, continue_on_error=False)
        specs = [NS(run_id=str(index), output_dir=Path(str(index))) for index in range(4)]
        wait = module.concurrent.futures.wait

        def completed_batch(futures, **kwargs):
            done, pending = wait(futures)
            # Expose the success first, so an early refill would start extra work.
            return sorted(done, key=lambda future: future.result().return_code), pending

        def execute(options, spec):
            return module.ModelRunResult(spec, int(spec.run_id == "1"))

        with (
            patch.object(module.concurrent.futures, "wait", side_effect=completed_batch),
            patch.object(module, "_run_one", side_effect=execute) as command,
        ):
            for keep_going in (False, True):
                options.continue_on_error = keep_going
                command.reset_mock()
                results = {result.spec.run_id: result for result in module.run_model_runs(options, specs)}
                self.assertEqual(command.call_count, 4 if keep_going else 2)
                self.assertEqual(len(results), 4)
                if not keep_going:
                    for index in ("2", "3"):
                        self.assertEqual(results[index].skip_reason, "not_started_after_failure")

    def test_execution_modes_and_failure_logs(self):
        with TemporaryDirectory(dir=ROOT_DIR) as temporary:
            options = NS(
                dry_run=False,
                model_run_jobs=1,
                continue_on_error=True,
                diagnostics=DiagnosticLevel("off"),
                trace_threads=1,
                trace_file_threads=1,
                hicache_io_model=NS(narrow_config=lambda *args: {}),
            )
            spec = self.spec(temporary)
            with (
                patch.object(module, "run_command") as command,
                patch("markov_internal.modeling.backend.trace_graph_executable", return_value=Path("trace_graph")),
            ):

                def execute(command_args, *, log_path):
                    retained = ModelingRunConfig.load(spec.output_dir / "runner_config.json")
                    self.assertEqual(command_args, module.build_trace_graph_command(retained))
                    self.assertFalse(details.exists())
                    if options.diagnostics.keep_debug_artifacts:
                        module.write_json(details, {"current": True})
                    log_path.write_text("sample failure or success output\n" * 4000)
                    return NS(returncode=code)

                command.side_effect = execute
                for dry, code, debug in ((True, 0, False), (False, 0, False), (False, 0, True), (False, 2, False)):
                    options.dry_run = dry
                    options.diagnostics = DiagnosticLevel("full" if debug else "off")
                    details = spec.output_dir / "model_summary.json"
                    module.write_json(details, {"previous": True})
                    command.reset_mock()
                    (result,) = module.run_model_runs(options, [spec])
                    self.assertEqual(result.return_code, code)
                    self.assertEqual(result.skip_reason, "dry_run" if dry else "")
                    self.assertEqual(command.call_count, int(not dry))
                    self.assertEqual(details.exists(), dry or debug)
                    if dry:
                        self.assertEqual(module.load_json(details), {"previous": True})
                    self.assertTrue((spec.output_dir / "runner_config.json").is_file())
                    log = spec.output_dir / "model.log"
                    self.assertEqual(log.exists(), debug or code != 0)
                    if code:
                        self.assertLessEqual(log.stat().st_size, 64 * 1024)
                options.continue_on_error = False
                following = NS(
                    run_id="following", label="following", output_dir=Path(temporary) / "following", skip_reason=""
                )
                command.reset_mock()
                results = {result.spec.run_id: result for result in module.run_model_runs(options, [spec, following])}
                self.assertEqual(results["cell"].return_code, 2)
                self.assertEqual(results["following"].skip_reason, "not_started_after_failure")
                self.assertEqual(command.call_count, 1)
