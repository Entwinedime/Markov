"""Small semantic regressions; run explicitly with python -m unittest.

These use no trace files, target scores, inference server or parameter search.
"""

from __future__ import annotations

import unittest
from pathlib import Path

from ...evaluation.scoring import observed_direct_cost
from ...calibration.runtime_anchors import _service_model
from ...io_model import HiCacheIoModel
from ...io_model_builder import _io_coverage, _new_write_curve, _physical_service, _row_prediction
from ...io_model_contract import io_observation_ready
from .oracle_cost_replay.matching import _oracle_cost_record, target_operation_cell
from .oracle_cost_replay.runner import _identity_mismatches, _select_replay_scores
from .phase.score import _compare_phase_cost
from ..final_dag.shape_compare import compare_shape
from ..final_dag.shape_oracle import _annotate_family_relations


def prefetch_observation(*, pages: int, completed: int) -> dict:
    return {
        "record_id": "read", "kind": "prefetch", "direction": "storage_to_host", "status": "ready",
        "pid": "worker", "resource_scope": "rank", "source_start_us": 10, "timing_fact_node_id": 1,
        "operation_count": 1, "page_size": 32, "service_page_count": pages, "completed_tokens": completed,
        "service_observed": pages > 0, "service_us": 20 * pages,
        "storage_service_batches": [{"page_count": pages}] if pages else [],
        "terminal_control_observed": True, "terminal_explicit_cpu_us": 3,
    }


def new_write_rows(batches: tuple[int, ...]) -> list[dict]:
    rows = []
    for page_size in (1, 2):
        for factor in (1, 2):
            pages = [factor * count for count in batches]
            byte_count = sum(pages) * page_size
            rows.append({
                "source_manifest": f"page_{page_size}", "family": "write_host_to_storage",
                "physical_new_us": 1., "physical_existing_us": 0.,
                "page_size": page_size, "page_count": sum(pages), "byte_count": byte_count,
                "observed_service_us": 10 * len(pages) + byte_count,
                "storage_new_batch_pages": pages,
            })
    return rows


class IoWorkContract(unittest.TestCase):
    def test_phase_oracle_keeps_decode_attention_component(self):
        costs = {name: {"source_duration_us": 10, "predicted_duration_us": 12}
                 for name in ("kernel_cost", "collective_cost", "submit_cost")}
        phase = {"logical_input": 0, "request_id": "request", **costs}
        work = {"cost_status": "ready", "prefills": [phase],
                "decodes": [{**phase, "predicted_paged_attention_duration_us": 3}]}
        observed = {"logical_input": 0, "request_ids": ["request"],
                    "prefill_common_kernel_duration_us": 7, "prefill_prefix_attention_duration_us": 4,
                    "decode_kernel_families": {"FusedInferAttentionScore": {"duration_us": 5},
                                               "MatMul": {"duration_us": 6}}}
        for name in ("prefill", "decode"):
            observed.update({f"{name}_kernel_duration_us": 11, f"{name}_collective_duration_us": 2,
                             f"{name}_submit_cpu_duration_us": 1})
        result = _compare_phase_cost(work, [observed], include_oracle_costs=True)
        for collection in ("device_oracle_costs", "all_owner_device_oracle_costs"):
            kernel = next(row for row in result[collection] if row["effect_id"].endswith(":decode:0:kernel"))
            self.assertEqual(kernel["duration_us"], 11)
            self.assertEqual(kernel["paged_attention_duration_us"], 5)
        self.assertNotIn("device_oracle_costs", _compare_phase_cost(work, [observed], include_oracle_costs=False))

    def test_background_service_status_is_model_input_ready(self):
        self.assertTrue(io_observation_ready({"status": "ready_background_unmaterialized"}))
        self.assertTrue(io_observation_ready({"status": "ready_background_transfer_only"}))
        self.assertFalse(io_observation_ready({"status": "unresolved"}))

    def test_cancelled_read_keeps_executed_service(self):
        row = prefetch_observation(pages=17, completed=0)
        self.assertEqual(observed_direct_cost(row), (340, 3))
        service = {"prefetch": {"setup_us_per_operation": 2., "setup_us_per_page": 0.,
                                "bandwidth_bytes_per_sec": 1_000_000.}}
        projected = _physical_service(row, service, 1)
        self.assertEqual(projected["page_count"], 17)
        self.assertEqual(projected["physical_service_us"], 546)

    def test_control_only_prefetch_has_no_service(self):
        self.assertEqual(observed_direct_cost(prefetch_observation(pages=0, completed=0)), (0, 3))

    def test_oracle_preserves_intrinsic_control_for_payload_and_zero_payload(self):
        for pages in (0, 17):
            record = {"effect_id": "prefetch", "effect_type": "prefetch_io_operation",
                      "direction": "storage_to_host", "resource_scope": "rank", "resource_lane": "read",
                      "logical_order_epoch": 0, "operation_count": 1, "page_count": pages,
                      "byte_count": pages * 64, "service_us": pages * 20, "control_us": 3}
            self.assertEqual(_oracle_cost_record(record, [record]), record)

    def test_identical_cost_replay_rejects_changed_scope_or_topology(self):
        original = dict.fromkeys(("node_count", "edge_count", "scope_owned_node_count", "scope_owned_node_duration_us",
                                  "scope_owned_gap_duration_us", "simulated_e2e_us", "simulated_gap_excluded_e2e_us",
                                  "gap_excluded_critical_path"), 1)
        self.assertEqual(_identity_mismatches(original, dict(original)), [])
        for field in original:
            replay = {**original, field: 2}
            self.assertEqual(_identity_mismatches(original, replay), [field])

    def test_oracle_uses_executed_pages_not_visible_pages(self):
        for pages, completed in ((17, 0), (17, 64), (0, 0)):
            with self.subTest(pages=pages, completed=completed):
                run = {"source_phase_observations": {"observations": [{"pid": "worker", "logical_input": 0}]},
                       "source_io_observations": {"observations": [prefetch_observation(pages=pages, completed=completed)]}}
                ledger = {"target_config_id": "target", "target_run_id": "run", "workload_id": "work"}
                row = target_operation_cell(ledger, run, 2)["by_kind"]["prefetch"]["records"][0]
                self.assertEqual(row["page_count"], pages)
                self.assertEqual(row["byte_count"], pages * 64)
                self.assertEqual(row["completed_page_count"], completed // 32)


class NewWriteCostContract(unittest.TestCase):
    def test_setup_is_charged_once_per_service_call(self):
        for batches in ((2,), (2, 2), (1, 3)):
            with self.subTest(batches=batches):
                rows = new_write_rows(batches)
                curve, _ = _new_write_curve(rows, 1)
                scales = {"write_host_to_storage_existing": [{"page_bytes": 1, "runtime_scale": 1.},
                                                              {"page_bytes": 2, "runtime_scale": 1.}]}
                for row in rows:
                    self.assertAlmostEqual(_row_prediction(row, scales, 1, curve), row["observed_service_us"])
                self.assertEqual(curve[0]["setup_us_per_operation"], 10.)

    def test_same_mean_batch_size_cannot_identify_two_coefficients(self):
        rows = new_write_rows((2,))
        for row in rows[1::2]:
            row["storage_new_batch_pages"] = [2, 2]
            row["observed_service_us"] = 20 + row["byte_count"]
        with self.assertRaisesRegex(ValueError, "bytes-per-call"):
            _new_write_curve(rows, 1)


class DmaCostContract(unittest.TestCase):
    def test_small_and_large_operations_identify_setup_and_rate(self):
        report = {"samples": [{"direction": "host_to_device", "page_bytes": 4, "operation_pages": pages,
                               "bytes": 4 * pages, "device_duration_us": 10 + 4 * pages}
                              for pages in (1, 10)]}
        model = _service_model(report, "host_to_device")
        point = model["page_bandwidth_points"][0]
        self.assertEqual(point["setup_us_per_operation"], 10.)
        self.assertEqual(point["bandwidth_bytes_per_sec"], 1_000_000.)
        for pages in (1, 10):
            row = {"kind": "load", "status": "ready", "service_observed": True, "service_page_count": pages,
                   "page_size": 4, "service_us": 10 + 4 * pages, "operation_count": 1,
                   "storage_service_batches": []}
            self.assertEqual(_physical_service(row, {"load": model}, 1)["physical_service_us"], 10 + 4 * pages)

    def test_negative_setup_is_not_a_negative_operation_cost(self):
        report = {"samples": [{"direction": "device_to_host", "page_bytes": 4, "operation_pages": pages,
                               "bytes": 4 * pages, "device_duration_us": 4 * pages - 1}
                              for pages in (1, 10)]}
        point = _service_model(report, "device_to_host")["page_bandwidth_points"][0]
        self.assertEqual(point["setup_us_per_operation"], 0.)
        self.assertGreater(point["bandwidth_bytes_per_sec"], 0.)


class IoCoverageContract(unittest.TestCase):
    def test_prediction_reports_observed_domain_without_changing_cost(self):
        coverage = _io_coverage([
            {"family": "load", "page_count": 1, "byte_count": 32, "service_call_bytes": [32.]},
            {"family": "load", "page_count": 1, "byte_count": 128, "service_call_bytes": [128.]},
        ])
        model = HiCacheIoModel(
            Path("model.json"),
            {"kv_bytes_per_token_per_rank": 1, "io_coverage": coverage},
        )
        inside = model.domain_status(32, {"load": {"records": [{"operation_count": 1, "byte_count": 64}]}})
        outside = model.domain_status(256, {"load": {"records": [{"operation_count": 1, "byte_count": 256}]}})
        self.assertEqual(inside["status"], "inside_observed_domain")
        self.assertEqual(outside["status"], "outside_observed_domain")


class ShapeContract(unittest.TestCase):
    def test_visibility_keeps_completed_pages_independently_of_waiting(self):
        for state in ("required", "not_required"):
            for completed in (0, 2, 17):
                transfer = {"effect_family_key": "request", "effect_type": "prefetch_io_operation",
                            "actual_state": "required", "effective_page_count": 17, "completed_page_count": completed}
                visibility = {"effect_family_key": "request", "effect_type": "prefetch_visibility_dependency",
                              "actual_state": state, "effective_page_count": 0, "completed_page_count": 0}
                _annotate_family_relations([transfer, visibility])
                self.assertEqual(visibility["effective_page_count"], completed)
                self.assertEqual(visibility["completed_page_count"], completed)
                self.assertEqual(visibility["blocking_relation"], "blocking" if state == "required" else "none")

    @staticmethod
    def row(*, pages: int, sensitivity: str = "schedule_invariant") -> dict:
        return {
            "effect_key": "effect", "effect_type": "prefetch_io_operation",
            "target_effect_state": "required", "actual_state": "required",
            "direction": "storage_to_host", "consumer_role": "background_completion",
            "blocking_relation": "background", "schedule_sensitivity": sensitivity,
            "operation_count": 1, "effective_page_count": pages,
            "completed_page_count": 0, "storage_existing_page_count": 0,
            "storage_new_page_count": 0, "storage_batches": [(0, pages, 0, 0)],
        }

    def test_work_difference_is_not_structure_exact(self):
        result = compare_shape(
            {"effects": [self.row(pages=1)], "lane_orders": {}},
            {"ready": True, "effects": [self.row(pages=100)], "lane_orders": {}},
        )
        self.assertFalse(result["acceptance_ready"])
        self.assertFalse(result["diagnostic_exact"])

    def test_schedule_difference_is_classified_but_not_exact(self):
        predicted = self.row(pages=1, sensitivity="arrival_schedule_sensitive")
        actual = {**predicted, "actual_state": "not_required", "target_effect_state": "not_required"}
        result = compare_shape(
            {"effects": [predicted], "lane_orders": {}},
            {"ready": True, "effects": [actual], "lane_orders": {}},
        )
        self.assertTrue(result["schedule_conditioned_ready"])
        self.assertFalse(result["acceptance_ready"])


class OracleReplaySelectionContract(unittest.TestCase):
    def test_limit_is_applied_after_strict_structure_selection(self):
        def row(model_run_id: str, *, structure_exact: bool) -> dict:
            return {
                "model_run_id": model_run_id,
                "status": "READY",
                "structure_exact": structure_exact,
                "phase_structure_exact": True,
            }

        selected, eligible_count = _select_replay_scores(
            [row("a_schedule_sensitive", structure_exact=False), row("z_exact", structure_exact=True)],
            1,
        )
        self.assertEqual(eligible_count, 1)
        self.assertEqual([item["model_run_id"] for item in selected], ["z_exact"])


if __name__ == "__main__":
    unittest.main()
