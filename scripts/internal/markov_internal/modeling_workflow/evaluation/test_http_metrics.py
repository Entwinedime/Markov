"""The full-window score must not silently become a scope or graph-end score."""

from pathlib import Path
import unittest
from unittest.mock import patch

from ...modeling.workload import WorkloadWindow
from .http_metrics import http_gates, http_metrics, score_http
from .scoring import score_cell, score_metrics
from ..io_model_contract import OPERATION_KINDS


def window(us):
    return WorkloadWindow(
        Path("bench/workload_report.json"), 1000, 1000 + int(us * 1000), int(us * 1000), "workload_report.formal_window"
    )


def cell(predicted=102, actual=100, base=130, *, name="cross", is_self=False):
    run = {
        "http_client": {"status": "connected", "e2e_us": predicted},
        "simulated_e2e_us": 9000,
        "simulated_gap_excluded_e2e_us": actual,
    }
    return {"model_run_id": name, "is_self": is_self, "full_e2e": score_http(run, (window(base),), (window(actual),))}


class HttpMetricsTests(unittest.TestCase):
    def test_execution_scores_http_without_inventing_components(self):
        prediction = {
            "model_run_id": "execution",
            "is_self": False,
            "status": "EXECUTED",
            "execution": {"cost_coverage": "partial"},
            "phase_modeled": True,
            "target_predicted": {"totals": None},
        }
        run = {"http_client": {"status": "connected", "e2e_us": 102}, "simulated_gap_excluded_e2e_us": None}
        scored = score_cell(
            prediction, run, {}, {}, "target", source_windows=(window(130),), target_windows=(window(100),)
        )
        self.assertAlmostEqual(scored["full_e2e"]["ape"], 0.02)
        self.assertIsNone(scored["direct"])
        self.assertIsNone(scored["structure_exact"])
        metrics = score_metrics([scored])
        self.assertTrue(metrics["gates"]["full_e2e"])
        self.assertEqual(metrics["status"], "MODEL_LIMITATION")
        self.assertEqual(metrics["direct"]["cell_count"], 0)
        self.assertIsNone(metrics["direct"]["absolute_error_us"])
        self.assertEqual(metrics["scope"]["cell_count"], 0)
        self.assertEqual(metrics["phase"], {})
        scored["phase_score"]["cost"]["metrics"]["prefill_compute"] = {
            "sample_count": 0,
            "wape": None,
            "p90_ape": None,
            "weighted_l1_us": None,
        }
        phase = score_metrics([scored])["phase"]["prefill_compute"]
        self.assertEqual(phase["cell_count"], 0)
        self.assertIsNone(phase["wape"])
        self.assertIsNone(phase["target_total_us"])
        self.assertIsNone(phase["weighted_l1_us"])
        with self.assertRaisesRegex(ValueError, "mapping"):
            score_cell(prediction, run, {}, {}, "target", include_oracle_costs=True)

    def test_mixed_rows_keep_missing_components_in_coverage(self):
        costs = {"service_us": 1, "control_us": 0}
        prediction = {
            "model_run_id": "legacy",
            "is_self": False,
            "status": "READY",
            "shape": {},
            "target_predicted": {"totals": costs, "by_kind": {kind: costs for kind in OPERATION_KINDS}},
        }
        run = {"http_client": {"status": "connected", "e2e_us": 102}, "source_io_observations": {"observations": []}}
        phase = {"structure_exact": True, "cost": {"ready": False, "metrics": {}}}
        with (
            patch(
                "markov_internal.modeling_workflow.evaluation.scoring.compare_shape",
                return_value={"acceptance_ready": True},
            ),
            patch("markov_internal.modeling_workflow.evaluation.scoring.compare_phase_work", return_value=phase),
        ):
            old = score_cell(
                prediction, run, run, {}, "target", source_windows=(window(130),), target_windows=(window(100),)
            )
        self.assertEqual(old["full_e2e"]["predicted_us"], 102)
        self.assertEqual(old["full_e2e"]["base_us"], 130)
        self.assertEqual(old["full_e2e"]["target_us"], 100)

        new = score_cell(
            {**prediction, "model_run_id": "execution", "status": "EXECUTED", "execution": {}},
            run,
            {},
            {},
            "target",
            source_windows=(window(130),),
            target_windows=(window(100),),
        )
        metrics = score_metrics([old, new])
        self.assertEqual(metrics["full_e2e"]["cell_count"], 2)
        self.assertEqual(metrics["direct"]["cell_count"], 1)
        self.assertEqual(metrics["direct"]["expected_cell_count"], 2)
        self.assertFalse(metrics["gates"]["direct"])

    def test_only_client_prediction_and_bench_truth(self):
        score = cell()["full_e2e"]
        self.assertEqual(score["predicted_us"], 102)
        self.assertEqual(score["target_us"], 100)
        self.assertEqual(score["exclusions"], [])
        self.assertAlmostEqual(score["ape"], 0.02)
        self.assertTrue(all(http_gates(http_metrics([cell()])).values()))

    def test_missing_http_never_falls_back_to_whole_graph(self):
        run = {"http_client": {"status": "missing_or_ambiguous_tokenizer_submit"}, "simulated_e2e_us": 100}
        result = score_http(run, (window(130),), (window(100),))
        self.assertFalse(result["ready"])
        self.assertNotIn("predicted_us", result)

    def test_formal_measurement_is_required(self):
        run = {"http_client": {"status": "connected", "e2e_us": 100}}
        for source, target in ((None, window(100)), (window(130), None), (window(130), window(0))):
            self.assertFalse(score_http(run, (source,), (target,))["ready"])
        inferred = WorkloadWindow(Path("report.json"), 0, 1000, 1000, "workload_report")
        self.assertFalse(score_http(run, (window(130),), (inferred,))["ready"])
        for value in (None, 0, -1, True, float("nan"), float("inf")):
            run["http_client"]["e2e_us"] = value
            self.assertFalse(score_http(run, (window(130),), (window(100),))["ready"])

    def test_repetitions_average_durations_without_collapsing_sample_evidence(self):
        run = {"http_client": {"status": "connected", "e2e_us": 105}}
        result = score_http(run, (window(120), window(140)), (window(90), window(110)))
        self.assertEqual(result["target_us"], 100)
        self.assertEqual(result["base_us"], 130)
        self.assertEqual(result["target_sample_count"], 2)
        self.assertEqual(result["target_range_us"], [90, 110])
        self.assertAlmostEqual(result["ape"], 0.05)
        self.assertFalse(score_http(run, (), (window(100),))["ready"])

    def test_cell_errors_do_not_cancel(self):
        rows = [cell(110, name="over"), cell(90, name="under")]
        result = http_metrics(rows)
        self.assertAlmostEqual(result["wape"], 0.1)
        self.assertEqual(len(result["over_5pct"]), 2)
        self.assertEqual(result["absolute_error_us"], 20)
        self.assertFalse(http_gates(result)["full_e2e"])
        ranked = http_metrics([cell(100 + error, name=str(error)) for error in range(1, 11)])
        self.assertAlmostEqual(ranked["p90_ape"], 0.09)

    def test_missing_cell_stays_in_coverage_gate(self):
        rows = [cell(), {"model_run_id": "missing", "is_self": False}]
        result = http_metrics(rows)
        self.assertEqual(result["cell_count"], 1)
        self.assertEqual(result["expected_cell_count"], 2)
        self.assertFalse(result["coverage_complete"])
        self.assertFalse(any(http_gates(result).values()))
        self.assertEqual(result["missing_cells"][0]["model_run_id"], "missing")

    def test_cross_must_improve_base_but_self_need_not(self):
        self.assertFalse(http_gates(http_metrics([cell(102, base=101)]))["base_wall"])
        self.assertTrue(http_gates(http_metrics([cell(102, base=100, is_self=True)]))["base_wall"])

    def test_formal_aggregator_includes_http_gates(self):
        # No rows cannot pass via empty all() or be presented as perfect E2E.
        result = score_metrics([])
        self.assertIn("full_e2e", result)
        self.assertFalse(result["gates"]["full_e2e"])
        self.assertFalse(result["gates"]["base_wall"])
        self.assertEqual(result["status"], "MODEL_LIMITATION")
