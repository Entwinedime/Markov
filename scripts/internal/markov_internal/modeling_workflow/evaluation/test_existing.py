"""Completed execution can be scored without pretending its cost projection exists."""

from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from . import existing
from .test_http_metrics import window


class ExistingExecutionTests(unittest.TestCase):
    def fixture(self, directory):
        execution = {
            "status": "executed",
            "prepared_facts": 12,
            "consumed_facts": 12,
            "confirmations": {"partial_window_rounds": 0},
            "cost_coverage": "partial",
        }
        evidence = {
            "model_run_id": "run",
            "source_config_id": "base",
            "target_config": "target",
            "workload_id": "workload",
            "is_self": False,
            "status": "EXECUTED",
            "target_hicache": {"page_size": 64},
            "source_manifest": "source.json",
        }
        run = {
            "prediction": evidence,
            "module_results": {"hicache_execution": execution},
            "http_client": {"status": "connected", "e2e_us": 102},
        }
        model = directory / "model_runs" / "run"
        documents = {
            directory / "workflow_summary.json": {
                "prediction": {"status": "EXECUTED", "completed_count": 1},
                "model_run_error_count": 0,
            },
            directory / "artifacts" / "model_run_plan.json": {
                "runs": [{"run_id": "source", "manifest_path": "source.json"}],
                "model_runs": [
                    {
                        "run_id": "run",
                        "source_run_id": "source",
                        "source_config_id": "base",
                        "target_config": "target",
                        "input_id": "workload",
                        "output_dir": str(model),
                    }
                ],
            },
            model / "run_summary.json": run,
        }
        return documents, evidence, run

    def test_execution_requires_its_own_complete_evidence(self):
        with TemporaryDirectory() as root:
            directory = Path(root)
            documents, evidence, run = self.fixture(directory)
            with patch.object(existing, "load_json", side_effect=lambda path: documents[path]):
                ((prediction, _),) = existing.completed_predictions(directory)
                self.assertIs(prediction["execution"], run["module_results"]["hicache_execution"])
                for field, value in (("consumed_facts", 11), ("status", "incomplete")):
                    original = run["module_results"]["hicache_execution"][field]
                    run["module_results"]["hicache_execution"][field] = value
                    with self.assertRaisesRegex(ValueError, "incomplete"):
                        existing.completed_predictions(directory)
                    run["module_results"]["hicache_execution"][field] = original
                evidence["target_config"] = "another"
                with self.assertRaisesRegex(ValueError, "another cell"):
                    existing.completed_predictions(directory)
                run.pop("prediction")
                with self.assertRaisesRegex(ValueError, "lacks retained scoring evidence"):
                    existing.completed_predictions(directory)

                # Current results have one identity record and no task index or
                # metadata written back into the C++ result.
                evidence["target_config"] = "target"
                documents[directory / "workflow_summary.json"]["prediction"]["cells"] = [evidence]
                del documents[directory / "artifacts" / "model_run_plan.json"]
                ((prediction, _),) = existing.completed_predictions(directory)
                self.assertIs(prediction["execution"], run["module_results"]["hicache_execution"])
                self.assertNotIn("execution", evidence)
                run["module_results"]["hicache_execution"]["consumed_facts"] = 11
                with self.assertRaisesRegex(ValueError, "incomplete"):
                    existing.completed_predictions(directory)

    def test_retained_static_scoring_evidence_remains_readable(self):
        with TemporaryDirectory() as root:
            directory = Path(root)
            documents, evidence, run = self.fixture(directory)
            evidence.update(status="READY", shape={"effects": [{}]})
            run["module_results"] = {
                "hicache_dag_patch": {
                    "phase_owner_conflict_count": 0,
                    "status": "applied",
                    "topology_valid": True,
                    "phase_patch_status": "ready",
                }
            }
            run["source_io_observations"] = {}
            with patch.object(existing, "load_json", side_effect=lambda path: documents[path]):
                self.assertEqual(len(existing.completed_predictions(directory)), 1)

    def test_http_evaluation_does_not_extract_target_dag(self):
        with TemporaryDirectory() as root:
            directory = Path(root) / "predictions"
            _, evidence, run = self.fixture(directory)
            target = SimpleNamespace(
                config_id="target",
                input_id="workload",
                hicache_config={"page_size": 64},
                run_id="target_run",
                label="target",
                manifest_path=Path("target.json"),
                config_path=Path("settings.json"),
                workload_window=window(100),
            )
            prediction = {**evidence, "execution": run["module_results"]["hicache_execution"]}
            with (
                patch.object(existing, "completed_predictions", return_value=[(prediction, run)]),
                patch.object(existing, "discover_profile_runs", return_value=[target]) as discover,
                patch.object(existing, "discover_workload_window", return_value=window(130)),
                patch.object(existing, "extract_target_phase_observation") as extract,
                patch.object(existing, "extract_target_shape_oracle") as shape,
            ):
                result = existing.evaluate([directory], [], [], Path(root) / "scores")
                self.assertAlmostEqual(result["full_e2e"]["wape"], 0.02)
                self.assertEqual(result["status"], "MODEL_LIMITATION")
                self.assertEqual(result["new_target_extractions"], 0)
                self.assertEqual(result["http_only_target_count"], 1)
                self.assertEqual(result["target_observations_reused"], 0)
                extract.assert_not_called()
                shape.assert_not_called()
                discover.reset_mock()
                with self.assertRaisesRegex(ValueError, "mapping"):
                    existing.evaluate([directory], [], [], Path(root) / "oracle", oracle_cost_replay=True)
                discover.assert_not_called()

                # Normal repetitions use a normal base reference, never the
                # profiled source wall time. Scoring does not extract a DAG.
                repeated = SimpleNamespace(**{**vars(target), "run_id": "repeat", "workload_window": window(110)})
                base = SimpleNamespace(**{**vars(target), "config_id": "base", "workload_window": window(125)})
                settings = {"profiling": {"enabled": False, "channels": []}}
                report = {
                    "status": "completed",
                    "formal_window": {"status": "ok"},
                    "requests": [{"measure": True, "http_status": 200, "logical_request_id": "request"}],
                }
                run["http_client"]["requests"] = [{"request_id": "request"}]
                discover.return_value = [target, repeated, base]
                with (
                    patch.object(
                        existing,
                        "load_json",
                        side_effect=lambda path: settings if path == target.config_path else report,
                    ),
                    patch.object(existing, "parse_profile_run", return_value=base),
                ):
                    result = existing.evaluate([directory], [], [], Path(root) / "normal", normal_http=True)
                    self.assertAlmostEqual(result["full_e2e"]["wape"], 3 / 105)
                    self.assertAlmostEqual(result["full_e2e"]["base_wall_wape"], 20 / 105)
                    self.assertEqual(result["measurement_sample_count"], 3)
                    settings["profiling"]["enabled"] = True
                    with self.assertRaisesRegex(ValueError, "instrumentation"):
                        existing.evaluate([directory], [], [], Path(root) / "invalid", normal_http=True)
                extract.assert_not_called()
