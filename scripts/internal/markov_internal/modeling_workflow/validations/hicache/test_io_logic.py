"""Small semantic regressions; run explicitly with python -m unittest.

These use no trace files, target scores, inference server or parameter search.
"""

from __future__ import annotations

import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
from unittest.mock import patch

from ....common.io import load_json, write_json

from ...evaluation.scoring import observed_direct_cost
from ...calibration.runtime_anchors import _service_model
from ...calibration import host_storage
from ...calibration.prefetch_service import capture_prefetch_timings, observe_prefetch_transfer, prefetch_io_bytes
from ...calibration.aggregation import build_prefetch_service_model
from ...io_model import HiCacheIoModel, interpolate_cost_curve
from ...io_model_builder import (
    _call_cost_curve,
    _physical_service,
    _io_coverage,
    observed_service_work,
    _service_model as fit_service,
    service_cost_evidence,
)
from ...io_model_validation import _service_model as validate_service
from ...io_model_contract import io_observation_ready
from .oracle_cost_replay.matching import target_operation_cell
from .oracle_cost_replay.runner import _identity_mismatches, _select_replay_scores, run_suite
from .phase.score import _compare_phase_cost
from ..final_dag.shape_compare import compare_shape
from ..final_dag.shape_oracle import _annotate_family_relations


class CostCurveTests(unittest.TestCase):
    def test_positive_scale_curve_and_endpoint_clamp(self):
        points = [{"size": 16, "cost": 8}, {"size": 1, "cost": 2}]
        for size, expected in [(0.5, 2), (1, 2), (4, 4), (16, 8), (32, 8)]:
            with self.subTest(size=size):
                self.assertAlmostEqual(interpolate_cost_curve(points, size, "size", "cost"), expected)

    def test_setup_curve_allows_zero_and_uses_linear_values(self):
        points = [{"size": 1, "cost": 0}, {"size": 16, "cost": 8}]
        self.assertEqual(interpolate_cost_curve(points, 4, "size", "cost", log_value=False), 4)


def prefetch_observation(*, pages: int, completed: int) -> dict:
    return {
        "record_id": "read",
        "kind": "prefetch",
        "direction": "storage_to_host",
        "status": "ready",
        "pid": "worker",
        "resource_scope": "rank",
        "source_start_us": 10,
        "timing_fact_node_id": 1,
        "operation_count": 1,
        "page_size": 32,
        "service_page_count": pages,
        "completed_tokens": completed,
        "service_observed": pages > 0,
        "service_us": 20 * pages,
        "storage_service_batches": [{"page_count": pages}] if pages else [],
        "terminal_control_observed": True,
        "terminal_explicit_cpu_us": 3,
    }


def new_write_rows(batches: tuple[int, ...]) -> list[dict]:
    rows = []
    for page_size in (1, 2):
        for factor in (1, 2):
            pages = [factor * count for count in batches]
            byte_count = sum(pages) * page_size
            rows.append(
                {
                    "source_manifest": f"page_{page_size}",
                    "family": "write_host_to_storage",
                    "storage_new_page_count": sum(pages),
                    "storage_existing_page_count": 0,
                    "page_size": page_size,
                    "page_count": sum(pages),
                    "byte_count": byte_count,
                    "observed_service_us": 10 * len(pages) + byte_count,
                    "storage_new_batch_pages": pages,
                }
            )
    return rows


class PrefetchServiceTiming(unittest.TestCase):
    def test_stage_parameters_follow_measured_work_not_total_elapsed(self):
        samples = []
        for size in (10, 20, 40):
            for pages in (1, 8):
                for copied in {1, pages}:
                    prefix = pages * (2 + 0.1 * size)
                    step = 3 + 0.2 * size
                    tail = 5 + 0.5 * pages
                    events = [{"stage": "read", "start_ns": 0, "end_ns": prefix * 1000}]
                    for page in range(copied):
                        start = prefix + page * step
                        events.extend(
                            [
                                {"stage": "copy", "start_ns": start * 1000, "end_ns": (start + step - 1) * 1000},
                                {
                                    "stage": "publish",
                                    "start_ns": (start + step - 1) * 1000,
                                    "end_ns": (start + step) * 1000,
                                },
                            ]
                        )
                    events.append(
                        {
                            "stage": "batch",
                            "page_count": pages,
                            "start_ns": 0,
                            "end_ns": (prefix + copied * step + tail) * 1000,
                        }
                    )
                    samples.append({"page_bytes": size, "events": events, "duration_ns": 999999999})
        model = build_prefetch_service_model(samples)
        expected = {
            "before_copy_us_per_page": 2,
            "before_copy_us_per_byte": 0.1,
            "copy_publish_us_per_page": 3,
            "copy_publish_us_per_byte": 0.2,
            "return_us_per_operation": 5,
            "return_us_per_page": 0.5,
        }
        for name, value in expected.items():
            self.assertAlmostEqual(model["stages"][name], value)
        self.assertEqual(set(model), {"direction", "stages"})
        single_page = build_prefetch_service_model([row for row in samples if row["page_bytes"] == 10])
        self.assertEqual(set(single_page), {"direction", "stages"})
        self.assertAlmostEqual(
            single_page["stages"]["before_copy_us_per_page"] + 10 * single_page["stages"]["before_copy_us_per_byte"],
            3.0,
        )
        fitted = validate_service(
            "prefetch", fit_service("prefetch", model, {"prefetch": [{"page_bytes": 10, "runtime_scale": 2}]}, [])
        )
        deployed = HiCacheIoModel(
            {
                "kv_bytes_per_token_per_rank": 1,
                "storage_batch_pages": 128,
                "service_models": {"prefetch": fitted},
                "control_models": {"prefetch": {"fixed_us_per_operation": 1, "state_check_us_per_operation": 2}},
                "phase_cost": {},
                "resource_lanes": {"storage_read": "scope", "storage_write": "scope"},
            },
        ).narrow_config(10, "wait_complete")
        self.assertEqual(deployed["io_cost"]["service_models"]["prefetch"]["stages"], fitted["stages"])
        self.assertEqual(deployed["io_cost"]["service_models"]["prefetch"]["runtime_scale"], 2)

        without_stages = {key: value for key, value in fitted.items() if key != "stages"}
        with self.assertRaisesRegex(ValueError, "stages"):
            validate_service("prefetch", without_stages)

    def test_prefetch_grid_accounts_for_work_and_cleans_failed_samples(self):
        reads, observed, cleared, charged = [], [], [], []

        def write(keys, page_bytes):
            reads.append("write")
            charged.append(page_bytes * sum(map(len, keys)))

        def observe(keys, page_bytes, *, cancel_after_pages):
            observed.append((page_bytes, len(keys[0]), cancel_after_pages))
            charged.append(2 * page_bytes * sum(map(len, keys)))
            return [{"duration_ns": 10, "requested_pages": len(keys[0]), "cancel_after_pages": cancel_after_pages}] * 2

        workers = SimpleNamespace(
            worker_storage_batch_pages=[128, 128],
            worker_devices=[4, 5],
            worker_numa_nodes=[0, 0],
            worker_threads=[1, 1],
            write=write,
            clear=lambda: cleared.append(True),
            observe_prefetch=observe,
        )
        rows = capture_prefetch_timings(workers, [2, 4, 8], warmup=1, repeats=2)
        self.assertEqual(sum(charged), prefetch_io_bytes([2, 4, 8], 128, 2, 1, 2))
        self.assertEqual(len(rows), 18 * 2 * 2)
        self.assertEqual(reads, ["write"] * 7)
        self.assertEqual(len(cleared), 7)
        self.assertEqual(
            {(row["page_bytes"], row["requested_pages"]) for row in rows},
            {(2, 1), (2, 8), (2, 129), (4, 1), (4, 8), (8, 1), (8, 8)},
        )
        self.assertEqual({row["ordinal"] for row in rows}, {0, 1})
        self.assertEqual({cancel for size, pages, cancel in observed if (size, pages) == (2, 129)}, {None, 0, 128})

        for before_failure in ([], [[]]):
            with patch.object(workers, "observe_prefetch", side_effect=[*before_failure, IOError("failed")]):
                with self.assertRaises(IOError):
                    capture_prefetch_timings(workers, [2], warmup=0, repeats=1)
        self.assertEqual(len(cleared), 9)  # Both warm-read and measured-read failure clear the keys.

    def run_sample(self, cancel=None, backend=None):
        class Operation:
            hash_value = ["a", "b", "c"]
            host_indices = [0, 1, 2]
            completed_tokens = 0
            terminated = False

            def increment(self, tokens):
                if self.terminated:
                    return False
                self.completed_tokens += tokens
                return True

            def mark_terminate(self):
                self.terminated = True

        class Controller:
            page_size = 1
            has_draft = False
            mem_pool_host = SimpleNamespace(
                get_dummy_flat_data_page=lambda: object(), set_from_flat_data_page=lambda index, page: None
            )
            storage_backend = backend or SimpleNamespace(batch_get=lambda keys, destinations: destinations)

            def generic(self, operation, keys, indices, extra):
                destinations = [self.mem_pool_host.get_dummy_flat_data_page() for key in keys]
                pages = self.storage_backend.batch_get(keys, destinations)
                for index, page in zip(indices, pages):
                    self.mem_pool_host.set_from_flat_data_page(index, page)
                    if not operation.increment(1):
                        break

            def transfer(self, operation):
                for start in range(0, 3, 2):
                    keys = operation.hash_value[start : start + 2]
                    before = operation.completed_tokens
                    self.page_get_func(operation, keys, operation.host_indices[start : start + 2], None)
                    if operation.completed_tokens != before + len(keys):
                        operation.mark_terminate()
                        break

        operation, controller = Operation(), Controller()
        controller.page_get_func = controller.generic
        ticks = iter(range(100, 1000))
        row = observe_prefetch_transfer(
            controller, operation, Controller.transfer, cancel_after_pages=cancel, clock=lambda: next(ticks)
        )
        self.assertEqual(controller.page_get_func, controller.generic)
        self.assertEqual(controller.mem_pool_host, Controller.mem_pool_host)
        self.assertTrue(all(0 <= event["start_ns"] < event["end_ns"] <= row["duration_ns"] for event in row["events"]))
        leaves = [event for event in row["events"] if event["stage"] != "batch"]
        self.assertEqual(
            row["duration_ns"], row["unattributed_ns"] + sum(event["end_ns"] - event["start_ns"] for event in leaves)
        )
        self.assertGreaterEqual(row["unattributed_ns"], 0)
        return row

    def test_success_and_cancelled_branch_counts(self):
        for cancel, expected in ((None, (3, 3, 3)), (0, (2, 1, 0)), (1, (2, 2, 1)), (2, (3, 3, 2))):
            with self.subTest(cancel=cancel):
                row = self.run_sample(cancel)
                self.assertEqual(tuple(row[key] for key in ("read_pages", "copied_pages", "published_pages")), expected)
                publications = [event for event in row["events"] if event["stage"] == "publish"]
                self.assertEqual(sum(event["accepted"] for event in publications), expected[2])
                self.assertEqual(
                    sum(event["stage"] == "injected_cancel" for event in row["events"]), int(cancel in (1, 2))
                )

    def test_nested_batches_are_not_added_to_costs(self):
        row = self.run_sample()
        batches = [event for event in row["events"] if event["stage"] == "batch"]
        self.assertEqual([event["page_count"] for event in batches], [2, 1])
        for event in row["events"]:
            if event["stage"] == "batch":
                continue
            self.assertEqual(
                sum(batch["start_ns"] <= event["start_ns"] <= event["end_ns"] <= batch["end_ns"] for batch in batches),
                1,
            )

    def test_no_result_on_failed_backend(self):
        def fail(*args):
            raise IOError("read failed")

        with self.assertRaisesRegex(IOError, "read failed"):
            self.run_sample(backend=SimpleNamespace(batch_get=fail))

    def test_invalid_cancellation_is_not_run(self):
        for cancel in (-1, 3):
            with self.subTest(cancel=cancel), self.assertRaises(ValueError):
                self.run_sample(cancel)


class StorageWriteBatchContract(unittest.TestCase):
    def test_sample_reports_runtime_batches_not_page_interleaving(self):
        calls = []

        def write(keys, page_bytes, operation_bytes_per_scope=0):
            calls.append("write")
            return [10, 20], 25

        workers = SimpleNamespace(
            write=write,
            clear=lambda: None,
            worker_threads=[1, 1],
            worker_devices=[4, 5],
            worker_numa_nodes=[0, 0],
            worker_storage_batch_pages=[2, 2],
        )
        row = host_storage.timed_new_write_batch(
            page_bytes=4,
            operation_bytes_per_scope=8,
            bytes_per_scope=12,
            scope_count=2,
            ordinal=0,
            process_scopes=workers,
        )
        self.assertEqual(calls, ["write"])
        self.assertEqual(row["operation_count"], 4)
        self.assertEqual(row["batch_semantics"], "runtime_materialize_then_batch_set")
        self.assertEqual(row["service_duration_ns"], 30)


class IoWorkContract(unittest.TestCase):
    def test_phase_oracle_keeps_decode_attention_component(self):
        costs = {
            name: {"source_duration_us": 10, "predicted_duration_us": 12}
            for name in ("kernel_cost", "collective_cost", "submit_cost")
        }
        phase = {"logical_input": 0, "request_id": "request", **costs}
        work = {
            "cost_status": "ready",
            "prefills": [phase],
            "decodes": [{**phase, "predicted_paged_attention_duration_us": 3}],
        }
        observed = {
            "logical_input": 0,
            "request_ids": ["request"],
            "prefill_common_kernel_duration_us": 7,
            "prefill_prefix_attention_duration_us": 4,
            "decode_kernel_families": {"FusedInferAttentionScore": {"duration_us": 5}, "MatMul": {"duration_us": 6}},
        }
        for name in ("prefill", "decode"):
            observed.update(
                {
                    f"{name}_kernel_duration_us": 11,
                    f"{name}_collective_duration_us": 2,
                    f"{name}_submit_cpu_duration_us": 1,
                }
            )
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
        row["storage_service_batches"][0]["copied_page_count"] = 1
        service = {
            "prefetch": {
                "stages": dict(
                    before_copy_us_per_page=0.0,
                    before_copy_us_per_byte=1.0,
                    copy_publish_us_per_page=0.0,
                    copy_publish_us_per_byte=0.0,
                    return_us_per_operation=2.0,
                    return_us_per_page=0.0,
                )
            }
        }
        projected = _physical_service(row, service, 1)
        self.assertEqual(projected["page_count"], 17)
        self.assertEqual(projected["physical_service_us"], 546)

    def test_control_only_prefetch_has_no_service(self):
        self.assertEqual(observed_direct_cost(prefetch_observation(pages=0, completed=0)), (0, 3))

    def test_staged_base_service_uses_copies_not_completed_tokens(self):
        stages = dict(
            before_copy_us_per_page=2.0,
            before_copy_us_per_byte=0.1,
            copy_publish_us_per_page=3.0,
            copy_publish_us_per_byte=0.2,
            return_us_per_operation=5.0,
            return_us_per_page=0.5,
        )
        service = {"prefetch": {"stages": stages, "bandwidth_bytes_per_sec": 1e6}}
        row = prefetch_observation(pages=4, completed=0)
        row["storage_service_batches"] = [{"page_count": 4, "copied_page_count": 1, "published_page_count": 0}]
        measured = _physical_service(row, service, 1)
        self.assertAlmostEqual(measured["physical_service_us"], 4 * (2 + 0.1 * 32) + (3 + 0.2 * 32) + 5 + 0.5 * 4)
        self.assertEqual(measured["copied_page_count"], 1)
        # Cross-rank visible counters never determine this rank's work.
        row["completed_tokens"] = 64
        self.assertEqual(_physical_service(row, service, 1), measured)
        row["storage_service_batches"] = [
            {"page_count": 2, "copied_page_count": 2},
            {"page_count": 2, "copied_page_count": 1},
        ]
        self.assertAlmostEqual(
            _physical_service(row, service, 1)["physical_service_us"],
            4 * (2 + 0.1 * 32) + 3 * (3 + 0.2 * 32) + 2 * 5 + 0.5 * 4,
        )

    def test_staged_base_requires_measured_copy_work(self):
        row = prefetch_observation(pages=2, completed=64)
        service = {"prefetch": {"stages": {}, "bandwidth_bytes_per_sec": 1e6}}
        for copied in (None, 0, 3):
            with self.subTest(copied=copied):
                row["storage_service_batches"] = [{"page_count": 2, "copied_page_count": copied}]
                self.assertIsNone(_physical_service(row, service, 1))

    def test_identical_cost_replay_rejects_changed_scope_or_topology(self):
        original = dict.fromkeys(
            (
                "node_count",
                "edge_count",
                "scope_owned_node_count",
                "scope_owned_node_duration_us",
                "scope_owned_gap_duration_us",
                "simulated_e2e_us",
                "simulated_gap_excluded_e2e_us",
                "gap_excluded_critical_path",
            ),
            1,
        )
        self.assertEqual(_identity_mismatches(original, dict(original)), [])
        for field in original:
            replay = {**original, field: 2}
            self.assertEqual(_identity_mismatches(original, replay), [field])

    def test_oracle_uses_executed_pages_not_visible_pages(self):
        for pages, completed in ((17, 0), (17, 64), (0, 0)):
            with self.subTest(pages=pages, completed=completed):
                run = {
                    "source_phase_observations": {"observations": [{"pid": "worker", "logical_input": 0}]},
                    "source_io_observations": {
                        "observations": [prefetch_observation(pages=pages, completed=completed)]
                    },
                }
                ledger = {"target_config_id": "target", "target_run_id": "run", "workload_id": "work"}
                row = target_operation_cell(ledger, run, 2)["by_kind"]["prefetch"]["records"][0]
                self.assertEqual(row["page_count"], pages)
                self.assertEqual(row["byte_count"], pages * 64)
                self.assertEqual(row["completed_page_count"], completed // 32)


class NewWriteCostContract(unittest.TestCase):
    def test_new_write_fit_needs_observed_work_not_physical_new_write_curve(self):
        rows = []
        for pages in (2, 4):
            observed = dict(
                kind="write_host_to_storage",
                status="ready",
                service_observed=True,
                service_page_count=pages,
                service_us=10 + pages * 32,
                page_size=32,
                storage_residency_observed=True,
                storage_service_batches=[
                    dict(page_count=pages, storage_existing_page_count=0, storage_new_page_count=pages)
                ],
            )
            # No new-write coefficients or existing-key curve are needed for pure new pages.
            row = observed_service_work(observed, 1)
            self.assertEqual(row["storage_new_page_count"], pages)
            self.assertNotIn("physical_service_us", row)
            rows.append(dict(row, source_manifest="base", role="base"))
        curve, _ = _call_cost_curve(rows, 1)
        self.assertEqual(curve, [dict(page_bytes=32, setup_us_per_operation=10.0, bandwidth_bytes_per_sec=1e6)])

    def test_setup_is_charged_once_per_service_call(self):
        for batches in ((2,), (2, 2), (1, 3)):
            with self.subTest(batches=batches):
                rows = new_write_rows(batches)
                curve, _ = _call_cost_curve(rows, 1)
                self.assertEqual(curve[0]["setup_us_per_operation"], 10.0)
                self.assertEqual(curve[0]["bandwidth_bytes_per_sec"], 1e6)

    def test_same_mean_batch_size_cannot_identify_two_coefficients(self):
        rows = new_write_rows((2,))
        for row in rows[1::2]:
            row["storage_new_batch_pages"] = [2, 2]
            row["observed_service_us"] = 20 + row["byte_count"]
        with self.assertRaisesRegex(ValueError, "bytes-per-call"):
            _call_cost_curve(rows, 1)


class BaseServiceEvidence(unittest.TestCase):
    def inputs(self):
        physical = dict(
            kv_geometry={"kv_bytes_per_token_per_rank": 1},
            service_models={
                "prefetch": dict(
                    direction="storage_to_host",
                    stages=dict(
                        before_copy_us_per_page=1.0,
                        before_copy_us_per_byte=0.0,
                        copy_publish_us_per_page=0.0,
                        copy_publish_us_per_byte=0.0,
                        return_us_per_operation=0.0,
                        return_us_per_page=0.0,
                    ),
                ),
                "load": dict(
                    direction="host_to_device",
                    page_bandwidth_points=[dict(page_bytes=1, setup_us_per_operation=0, bandwidth_bytes_per_sec=1e6)],
                ),
                "write_device_to_host": dict(
                    direction="device_to_host",
                    page_bandwidth_points=[dict(page_bytes=1, setup_us_per_operation=0, bandwidth_bytes_per_sec=1e6)],
                ),
                "write_host_to_storage": dict(
                    direction="host_to_storage",
                    existing_key_bandwidth_points=[dict(page_bytes=1, operation_pages=1, bandwidth_bytes_per_sec=1e6)],
                ),
            },
        )
        rows = [
            dict(
                source_manifest="base",
                role="base",
                family=kind,
                page_size=1,
                page_count=4,
                byte_count=4,
                physical_service_us=4,
                storage_new_page_count=0,
                storage_existing_page_count=4 if kind == "write_host_to_storage" else 0,
                observed_service_us=8,
                service_call_bytes=[4],
            )
            for kind in physical["service_models"]
        ]
        rows += [
            dict(
                row,
                role="base",
                source_manifest="base",
                physical_service_us=row["observed_service_us"],
                service_call_bytes=[row["byte_count"]],
            )
            for row in new_write_rows((2,))
            if row["page_size"] == 1
        ]
        return physical, rows

    def evidence(self, physical, rows):
        with patch("markov_internal.modeling_workflow.io_model_builder.service_observation_rows", return_value=rows):
            return service_cost_evidence(physical, [])

    def test_base_wins_over_more_independent_samples_and_one_page_is_enough(self):
        physical, rows = self.inputs()
        rows += [
            dict(
                row,
                source_manifest="extra" + str(i),
                role="calibration",
                observed_service_us=100 * row["observed_service_us"],
            )
            for row in rows[:]
            for i in range(3)
        ]
        result = self.evidence(physical, rows)
        self.assertEqual(result["status"], "ready")
        self.assertTrue(all(item["evidence_origin"] == "base" for item in result["sources"].values()))
        self.assertEqual(result["models"]["load"]["runtime_scale_points"], [{"page_bytes": 1, "runtime_scale": 2}])
        self.assertEqual(len(result["models"]["write_host_to_storage"]["new_operation_points"]), 1)
        self.assertTrue(all(row["role"] == "base" for row in result["rows"]))

    def test_only_missing_family_uses_supplement(self):
        physical, rows = self.inputs()
        for row in rows:
            if row["family"] == "load":
                row.update(role="calibration", source_manifest="shared")
        result = self.evidence(physical, rows)
        self.assertEqual(result["status"], "ready")
        self.assertEqual(result["sources"]["load"]["evidence_origin"], "base_with_supplement")
        self.assertIsNotNone(result["sources"]["load"]["base_gap"])
        self.assertEqual(result["sources"]["prefetch"]["source_manifests"], ["base"])

    def test_base_dma_identifies_setup_and_rate_without_independent_curve(self):
        physical, rows = self.inputs()
        del physical["service_models"]["load"]
        small = next(row for row in rows if row["family"] == "load")
        small["observed_service_us"] = 14
        large = dict(small, page_count=8, byte_count=8, service_call_bytes=[8], observed_service_us=18)
        rows.append(large)

        result = self.evidence(physical, rows)
        self.assertEqual(result["status"], "ready")
        self.assertEqual(
            result["models"]["load"]["page_bandwidth_points"],
            [dict(page_bytes=1, setup_us_per_operation=10, bandwidth_bytes_per_sec=1e6)],
        )
        self.assertEqual(result["sources"]["load"]["evidence_origin"], "base")
        self.assertNotIn("load", physical["service_models"])
        validate_service("load", result["models"]["load"])

        large.update(role="calibration", source_manifest="shared")
        supplemented = self.evidence(physical, rows)
        self.assertEqual(supplemented["models"], result["models"])
        self.assertEqual(supplemented["sources"]["load"]["evidence_origin"], "base_with_supplement")
        self.assertIsNotNone(supplemented["sources"]["load"]["base_gap"])

        large["observed_service_us"] = 40  # Negative intercept must not force a base fit.
        limited = self.evidence(physical, rows)
        self.assertEqual(limited["missing"][0]["component"], "physical/load")

    def test_incomplete_physical_report_keeps_identifiable_base_costs(self):
        physical, rows = self.inputs()
        del physical["service_models"]["load"]
        del physical["service_models"]["write_host_to_storage"]
        result = self.evidence(physical, rows)
        self.assertEqual(result["status"], "data_limitation")
        self.assertEqual(
            {item["component"] for item in result["missing"]},
            {"physical/load", "physical/write_host_to_storage_existing"},
        )
        self.assertEqual(result["sources"]["write_host_to_storage_new"]["evidence_origin"], "base")
        self.assertEqual(set(result["models"]), {"prefetch", "write_device_to_host", "write_host_to_storage"})
        storage = result["models"]["write_host_to_storage"]
        self.assertEqual(set(storage), {"direction", "new_operation_points"})
        validate_service("write_host_to_storage", storage)

        observed = dict(
            kind="write_host_to_storage",
            status="ready",
            service_observed=True,
            service_us=12,
            service_page_count=2,
            page_size=1,
            storage_residency_observed=True,
            storage_service_batches=[dict(page_count=2, storage_existing_page_count=0, storage_new_page_count=2)],
        )
        row = _physical_service(observed, {}, 1)
        self.assertEqual(row["observed_service_us"], 12)
        self.assertEqual(row["storage_new_batch_pages"], [2])
        observed["storage_service_batches"][0].update(storage_existing_page_count=1, storage_new_page_count=1)
        self.assertIsNone(_physical_service(observed, {}, 1))

    def test_independent_new_write_parameters_only_fill_unidentified_cost(self):
        physical, rows = self.inputs()
        curve = [dict(page_bytes=1, setup_us_per_operation=99.0, bandwidth_bytes_per_sec=2e6)]
        physical["service_models"]["write_host_to_storage"]["new_operation_points"] = curve
        physical.update(
            measurement_sources=["shared/physical.json"], target_workload_trace_used=False, target_e2e_used=False
        )
        base_model = self.evidence(physical, rows)["models"]["write_host_to_storage"]
        self.assertEqual(base_model["new_operation_points"][0]["setup_us_per_operation"], 10.0)
        # Keep one base size: it cannot identify both setup and bandwidth.
        rows = rows[:-1]
        result = self.evidence(physical, rows)
        self.assertEqual(result["status"], "ready")
        self.assertEqual(result["models"]["write_host_to_storage"]["new_operation_points"], curve)
        source = result["sources"]["write_host_to_storage_new"]
        self.assertEqual(source["evidence_origin"], "independent_physical")
        self.assertEqual(source["measurement_sources"], ["shared/physical.json"])

    def test_one_extra_size_completes_base_new_write_not_a_full_calibration(self):
        physical, rows = self.inputs()
        rows[-1].update(role="calibration", source_manifest="one-extra-size")
        result = self.evidence(physical, rows)
        self.assertEqual(result["status"], "ready")
        source = result["sources"]["write_host_to_storage_new"]
        self.assertEqual(source["evidence_origin"], "base_with_supplement")
        self.assertEqual(source["source_manifests"], ["base", "one-extra-size"])
        self.assertEqual(
            result["models"]["write_host_to_storage"]["new_operation_points"][0]["setup_us_per_operation"], 10
        )

    def test_independent_services_do_not_require_redundant_workload_measurements(self):
        physical, rows = self.inputs()
        bandwidth = [dict(page_bytes=1, setup_us_per_operation=3, bandwidth_bytes_per_sec=1e6)]
        for family in ("load", "write_device_to_host"):
            physical["service_models"][family]["page_bandwidth_points"] = bandwidth
        storage = physical["service_models"]["write_host_to_storage"]
        storage["existing_key_bandwidth_points"] = [dict(page_bytes=1, operation_pages=1, bandwidth_bytes_per_sec=1e6)]
        storage["new_operation_points"] = bandwidth
        physical.update(
            measurement_sources=["shared/physical.json"], target_workload_trace_used=False, target_e2e_used=False
        )
        for family in physical["service_models"]:
            with self.subTest(family=family):
                result = self.evidence(physical, [row for row in rows if row["family"] != family])
                self.assertEqual(result["status"], "ready")
                for component, source in result["sources"].items():
                    expected = "independent_physical" if component.startswith(family) else "base"
                    self.assertEqual(source["evidence_origin"], expected)
                validate_service(family, result["models"][family])
                if family != "write_host_to_storage":
                    self.assertEqual(result["models"][family]["runtime_scale_points"][0]["runtime_scale"], 1)

                # Missing provenance cannot silently grant a runtime estimate.
                limited = self.evidence(
                    {**physical, "measurement_sources": []}, [row for row in rows if row["family"] != family]
                )
                self.assertEqual(
                    limited["missing"][0]["component"],
                    "service/" + family + ("_existing" if family == "write_host_to_storage" else ""),
                )

        result = self.evidence(physical, [])
        self.assertEqual(result["status"], "ready")
        self.assertEqual(result["rows"], [])
        self.assertIsNone(_io_coverage(result["rows"])["page_bytes"])

    def test_readiness_and_construction_share_service_evidence(self):
        from ...io_model_builder import prepare_model

        physical, rows = self.inputs()
        capture = dict(
            role="base",
            source_manifest="base",
            source_io_observations={"observations": []},
            source_phase_observations=dict(status="missing"),
        )
        with (
            TemporaryDirectory() as directory,
            patch("markov_internal.modeling_workflow.io_model_builder.service_observation_rows", return_value=rows),
        ):
            output = Path(directory)
            write_json(output / "model_build_summary.json", {"status": "ready"})
            readiness = prepare_model(
                SimpleNamespace(
                    physical=physical,
                    targets=[SimpleNamespace(label="target", fields={"prefetch_policy": "wait_complete"})],
                    raw={},
                    sources=[SimpleNamespace(hicache_config={"page_size": 128})],
                ),
                [capture],
                output=output,
            )
            self.assertFalse((output / "hicache_io_model.json").exists())
            summary = load_json(output / "model_build_summary.json")
            self.assertEqual(summary, {"status": "needs_calibration_data", "model_inputs": readiness})
        self.assertFalse(any(row["component"].startswith("service/") for row in readiness["missing"]))
        self.assertFalse(any(row["component"] == "execution_control/prefetch" for row in readiness["missing"]))
        self.assertFalse(any(row["component"].startswith("control/") for row in readiness["missing"]))
        self.assertEqual(readiness["service_evidence"], self.evidence(physical, rows)["sources"])

    def test_target_observations_are_not_cost_evidence(self):
        from ...io_model_builder import prepare_model
        from ...group import admitted_physical_calibration

        with self.assertRaisesRegex(ValueError, "never target"):
            prepare_model(None, [dict(role="target", source_manifest="target")])
        with self.assertRaisesRegex(ValueError, "manifest once"):
            prepare_model(None, [dict(role=role, source_manifest="same") for role in ("base", "calibration")])

        physical, _ = self.inputs()
        physical.update(
            resource_lanes={}, storage_batch_pages=128, target_workload_trace_used=False, target_e2e_used=False
        )
        with TemporaryDirectory() as directory:
            report = Path(directory) / "physical.json"
            source = Path(directory) / "samples.json"
            write_json(source, {})
            declaration = dict(
                report=str(report), measurement_sources=[str(source)], measurement_description="independent"
            )
            write_json(report, physical)
            self.assertEqual(admitted_physical_calibration(declaration)["service_models"], physical["service_models"])
            for field in ("target_workload_trace_used", "target_e2e_used"):
                for value in (True, None):
                    write_json(report, {**physical, field: value})
                    with self.subTest(field=field, value=value), self.assertRaisesRegex(ValueError, field):
                        admitted_physical_calibration(declaration)

    def test_total_prefetch_cost_is_not_a_substitute_for_stage_measurements(self):
        physical, rows = self.inputs()
        del physical["service_models"]["prefetch"]["stages"]
        result = self.evidence(physical, rows)
        self.assertEqual(result["status"], "data_limitation")
        self.assertEqual(result["missing"][0]["component"], "physical/prefetch_stages")
        self.assertNotIn("prefetch", result["models"])
        self.assertEqual(set(result["models"]), {"load", "write_device_to_host", "write_host_to_storage"})
        self.assertEqual(result["sources"]["load"]["evidence_origin"], "base")
        self.assertEqual(result["sources"]["write_host_to_storage_new"]["points"][0]["setup_us_per_operation"], 10)
        self.assertNotIn("prefetch", result["sources"])


class DmaCostContract(unittest.TestCase):
    def test_small_and_large_operations_identify_setup_and_rate(self):
        report = {
            "samples": [
                {
                    "direction": "host_to_device",
                    "page_bytes": 4,
                    "operation_pages": pages,
                    "bytes": 4 * pages,
                    "device_duration_us": 10 + 4 * pages,
                }
                for pages in (1, 10)
            ]
        }
        model = _service_model(report, "host_to_device")
        point = model["page_bandwidth_points"][0]
        self.assertEqual(point["setup_us_per_operation"], 10.0)
        self.assertEqual(point["bandwidth_bytes_per_sec"], 1_000_000.0)
        for pages in (1, 10):
            row = {
                "kind": "load",
                "status": "ready",
                "service_observed": True,
                "service_page_count": pages,
                "page_size": 4,
                "service_us": 10 + 4 * pages,
                "operation_count": 1,
                "storage_service_batches": [],
            }
            self.assertEqual(_physical_service(row, {"load": model}, 1)["physical_service_us"], 10 + 4 * pages)

    def test_negative_setup_is_not_a_negative_operation_cost(self):
        report = {
            "samples": [
                {
                    "direction": "device_to_host",
                    "page_bytes": 4,
                    "operation_pages": pages,
                    "bytes": 4 * pages,
                    "device_duration_us": 4 * pages - 1,
                }
                for pages in (1, 10)
            ]
        }
        point = _service_model(report, "device_to_host")["page_bandwidth_points"][0]
        self.assertEqual(point["setup_us_per_operation"], 0.0)
        self.assertGreater(point["bandwidth_bytes_per_sec"], 0.0)


class ShapeContract(unittest.TestCase):
    def test_visibility_keeps_completed_pages_independently_of_waiting(self):
        for state in ("required", "not_required"):
            for completed in (0, 2, 17):
                transfer = {
                    "effect_family_key": "request",
                    "effect_type": "prefetch_io_operation",
                    "actual_state": "required",
                    "effective_page_count": 17,
                    "completed_page_count": completed,
                }
                visibility = {
                    "effect_family_key": "request",
                    "effect_type": "prefetch_visibility_dependency",
                    "actual_state": state,
                    "effective_page_count": 0,
                    "completed_page_count": 0,
                }
                _annotate_family_relations([transfer, visibility])
                self.assertEqual(visibility["effective_page_count"], completed)
                self.assertEqual(visibility["completed_page_count"], completed)
                self.assertEqual(visibility["blocking_relation"], "blocking" if state == "required" else "none")

    @staticmethod
    def row(*, pages: int, sensitivity: str = "schedule_invariant") -> dict:
        return {
            "effect_key": "effect",
            "effect_type": "prefetch_io_operation",
            "target_effect_state": "required",
            "actual_state": "required",
            "direction": "storage_to_host",
            "consumer_role": "background_completion",
            "blocking_relation": "background",
            "schedule_sensitivity": sensitivity,
            "operation_count": 1,
            "effective_page_count": pages,
            "completed_page_count": 0,
            "storage_existing_page_count": 0,
            "storage_new_page_count": 0,
            "storage_batches": [(0, pages, 0, 0)],
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
    def test_no_exact_candidates_reports_not_run_without_opening_prediction_assets(self):
        score = dict(
            source_config_id="base", is_self=False, status="READY", structure_exact=False, phase_structure_exact=True
        )
        with TemporaryDirectory() as directory:
            result = run_suite({}, [score], {}, {}, Path(directory), source_config_id="base")

        self.assertEqual(result["status"], "NOT_RUN")
        self.assertEqual(result["excluded_nonexact_structure_count"], 1)
        self.assertEqual(result["cpp_replay_count"], 0)
        self.assertIsNone(result["direct_cost"]["total"]["wape_pct"])
        self.assertIsNone(result["direct_cost"]["delta_weighted_l1_pct"])
        self.assertFalse(result["direct_cost"]["total"]["passed"])

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
