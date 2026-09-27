"""Pairing rejects different inference work, not just different labels."""

import copy
import unittest

from .base_pair import validate_base_pair, validate_forward_work


class BasePairTests(unittest.TestCase):
    def inputs(self):
        config = {"server": {"tp": 2}, "env": {"DEVICE": "0,1"}, "profiling": {"enabled": False}}
        forced = dict(
            enabled=True,
            mode="replay",
            request_count=1,
            output_checked_count=1,
            all_actual_outputs_match_plan=True,
            unchecked_count=0,
            prompt_mismatch_count=0,
            mismatch_count=0,
        )
        row = dict(
            kind="request",
            http_status=200,
            actual_prompt_matches_plan=True,
            actual_output_matches_forced=True,
            logical_request_id="r",
            measure=True,
            actual_prompt_token_count=128,
            actual_output_count=2,
            forced_output_count=2,
            origin_input_count=128,
            anchor_tokens=128,
            tail_tokens=0,
        )
        report = {"workload_id": "work", "forced_token": forced, "requests": [row]}
        plan = {"requests": [{"prompt": [1], "output": [2]}]}
        return [config, copy.deepcopy(config)], [report, copy.deepcopy(report)], [plan, copy.deepcopy(plan)]

    def test_only_measurement_and_labels_may_differ(self):
        configs, reports, plans = self.inputs()
        configs[1].update(name="profiled", run_id="another_output", profiling={"enabled": True})
        configs[1]["server"]["startup_max_attempts"] = 3
        configs[1]["env"]["SGLANG_STEP_TIMING_DIR"] = "different-output"
        validate_base_pair(configs, reports, plans)
        configs[1]["server"]["tp"] = 4
        with self.assertRaises(ValueError):
            validate_base_pair(configs, reports, plans)

    def test_plan_and_actual_work_must_match(self):
        for case in ("plan", "output", "count", "unchecked", "identity"):
            configs, reports, plans = self.inputs()
            if case == "plan":
                plans[1]["requests"][0]["output"] = [3]
            elif case == "output":
                reports[1]["requests"][0]["actual_output_matches_forced"] = False
            elif case == "count":
                reports[1]["requests"][0]["actual_prompt_token_count"] = 64
            elif case == "identity":
                reports[1]["workload_id"] = "another"
            else:
                reports[1]["forced_token"]["unchecked_count"] = 1
            with self.subTest(case=case), self.assertRaises(ValueError):
                validate_base_pair(configs, reports, plans)

    def test_exact_shape_and_rank_not_timing(self):
        rows = [
            dict(
                request_id="r",
                identity={"tp_rank": 0},
                name="step[EXTEND bs=1 toks=128]",
                pid=1,
            )
        ]
        other = copy.deepcopy(rows)
        other[0]["pid"] = 200
        validate_forward_work(rows, other)
        other[0]["name"] = "step[EXTEND bs=1 toks=64]"
        with self.assertRaises(ValueError):
            validate_forward_work(rows, other)
