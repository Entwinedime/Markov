"""Independent phase measurements need not repeat a fixed endpoint workload."""

import unittest

from .phase_calibration import (
    _collapse_calibration_repeats,
    _intrinsic_requests,
    _token_curve,
    _fit_nonnegative,
    _regression_evidence,
    MissingPhaseEvidence,
    build_phase_cost,
)
from .io_model import _phase_cost


def experiment(source, new=32, duration=10.0, role="calibration"):
    return [
        dict(
            source_manifest=source,
            source_manifests=[source],
            role=role,
            request="request",
            rank=rank,
            page_size=128,
            prompt=256,
            new=new,
            context=256 - new,
            decode_iterations=8,
            common=duration,
            prefix=duration,
            prefill_collective=duration,
            decode_collective=duration,
            paged=duration,
        )
        for rank in (0, 1)
    ]


class PhaseRepeatTests(unittest.TestCase):
    def test_prefill_only_work_has_no_decode_parameters(self):
        observations = [
            dict(
                request_ids=[str(index)],
                logical_input=rank,
                source_page_size=32,
                prompt_token_count=prompt,
                prefill_token_count=new,
                prefill_common_kernel_duration_us=new * 2,
                prefill_prefix_attention_duration_us=3 + new + new * (prompt - new / 2) / 100,
                prefill_collective_duration_us=new,
                decode_iteration_count=0,
                decode_collective_duration_us=0,
            )
            for index, (new, prompt) in enumerate(((32, 64), (64, 128), (128, 160)))
            for rank in (0, 1)
        ]
        captures = [
            dict(
                role="base",
                source_manifest="base",
                source_phase_observations=dict(status="ready", observations=observations),
            )
        ]
        phase, summary = build_phase_cost(captures, 32)
        self.assertEqual(_phase_cost(phase), phase)
        self.assertIsNone(phase["decode_paged_attention"])
        self.assertIsNone(phase["decode_collective"])
        self.assertIsNone(phase["coverage"]["min_decode_context_tokens"])
        self.assertEqual(
            summary["parameter_sources"]["decode_collective"]["reason"], "base_workloads_have_no_decode_iterations"
        )
        observations[0]["decode_iteration_count"] = 1
        with self.assertRaisesRegex(ValueError, "kernel families"):
            build_phase_cost(captures, 32)

    def test_identical_work_collapses_with_real_provenance(self):
        rows = _collapse_calibration_repeats(experiment("a", duration=10.0) + experiment("b", duration=30.0))
        self.assertEqual(len(rows), 2)
        self.assertEqual(rows[0]["common"], 20.0)
        self.assertEqual(rows[0]["source_manifests"], ["a", "b"])
        self.assertEqual(rows[0]["source_manifest"], "a")
        self.assertEqual(len(_intrinsic_requests(rows)), 1)

    def test_same_page_different_work_is_not_rejected_or_merged(self):
        rows = _collapse_calibration_repeats(experiment("a") + experiment("b", new=128))
        self.assertEqual(len(rows), 4)
        self.assertEqual(len(_intrinsic_requests(rows)), 2)

    def test_base_is_not_averaged_with_independent_repeat(self):
        rows = _collapse_calibration_repeats(experiment("base", duration=5.0, role="base") + experiment("extra"))
        self.assertEqual(len(rows), 4)
        self.assertEqual(rows[0]["common"], 5.0)

    def test_missing_rank_is_not_repaired_by_another_capture(self):
        rows = _collapse_calibration_repeats(experiment("complete") + experiment("partial")[:1])
        with self.assertRaises(ValueError):
            _intrinsic_requests(rows)

    def test_two_base_anchors_do_not_consume_independent_costs(self):
        bases = experiment("base", new=32, duration=10.0, role="base") + experiment(
            "base", new=128, duration=20.0, role="base"
        )
        points, source = _token_curve(bases + experiment("extra", new=64, duration=100.0), "common")
        self.assertEqual(points, [dict(new_tokens=32, duration_us=10.0), dict(new_tokens=128, duration_us=20.0)])
        self.assertEqual(source["evidence_origin"], "base")

    def test_one_base_anchor_is_not_overwritten_by_supplement(self):
        rows = (
            experiment("base", duration=10.0, role="base")
            + experiment("same-size", duration=100.0)
            + experiment("new-size", new=128, duration=20.0)
        )
        points, source = _token_curve(rows, "common")
        self.assertEqual(points[0]["duration_us"], 10.0)
        self.assertEqual(source["evidence_origin"], "base_plus_independent")
        self.assertEqual({row["source_manifest"] for row in source["logical_anchors"]}, {"base", "new-size"})

    def test_one_size_cannot_identify_curve_slope(self):
        with self.assertRaises(MissingPhaseEvidence):
            _token_curve(experiment("base", role="base"), "common")

    def test_large_token_pair_units_do_not_hide_identifiable_fixed_cost(self):
        rows = [
            dict(fixed=1.0, new=n, attention=a, duration=7.0 + 2.0 * n + 0.003 * a)
            for n, a in [(32, 1e6), (128, 2e6), (512, 1e6), (1024, 8e6)]
        ]
        coefficients = _fit_nonnegative(rows, "duration", ("fixed", "new", "attention"))
        for name, expected in [("fixed", 7.0), ("new", 2.0), ("attention", 0.003)]:
            self.assertAlmostEqual(coefficients[name], expected, places=6)

    def test_changing_feature_units_preserves_predictions(self):
        rows = [dict(fixed=1.0, context=x, duration=y) for x, y in [(10, 12.0), (20, 22.0), (50, 53.0)]]
        rescaled = [dict(row, context=row["context"] * 1e6) for row in rows]
        ordinary = _fit_nonnegative(rows, "duration", ("fixed", "context"))
        large = _fit_nonnegative(rescaled, "duration", ("fixed", "context"))
        self.assertAlmostEqual(ordinary["fixed"], large["fixed"])
        self.assertAlmostEqual(ordinary["context"], large["context"] * 1e6)

    def test_regression_adds_only_independent_feature_information(self):
        base = experiment("base", new=32, role="base") + experiment("base", new=128, role="base")
        repeated = experiment("redundant", new=64)
        extra = [dict(row, context=400 - row["new"]) for row in experiment("extra", new=64)]
        rows = [dict(row, fixed=1.0) for row in base + repeated + extra]
        selected, evidence = _regression_evidence(rows, ("fixed", "new", "context"))
        self.assertEqual(evidence["base_feature_rank"], 2)
        self.assertEqual(evidence["selected_feature_rank"], 3)
        self.assertEqual(evidence["source_manifests"], ["base", "extra"])
        # Changing the recorded duration cannot influence sample selection.
        _, changed = _regression_evidence([dict(row, duration=1e9) for row in rows], ("fixed", "new", "context"))
        self.assertEqual(changed, evidence)

    def test_regression_keeps_base_and_reports_unresolved_coefficients(self):
        rows = [dict(row, fixed=1.0) for row in experiment("base", role="base") + experiment("repeat")]
        _, evidence = _regression_evidence(rows, ("fixed", "new", "context"))
        self.assertEqual(evidence["source_manifests"], ["base"])
        self.assertEqual(evidence["selected_feature_rank"], 1)
        self.assertIn("not separately identifiable", evidence["limitation"])

    def test_empty_regression_is_a_missing_measurement_not_a_zero_cost(self):
        with self.assertRaises(MissingPhaseEvidence):
            _regression_evidence([], ("fixed", "new", "context"))
