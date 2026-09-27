"""Shared templates follow declared branches, not target-specific costs."""

import unittest

from ..control_calibrations import (
    CALIBRATION_FIELDS,
    control_calibrations,
    select_control_calibrations,
)
from ..execution.runner_adapter import hicache_model_config
from ..io_model import HiCacheIoModel
from ..types import TargetHiCacheConfig
from .target_configs import parse_target_config


class ControlCalibrationTests(unittest.TestCase):
    def test_target_default_policy_does_not_inherit_base_policy(self):
        targets = [
            TargetHiCacheConfig(name, {"prefetch_policy": policy})
            for name, policy in [("a", "timeout"), ("b", "wait_complete"), ("c", "best_effort")]
        ]
        targets.append(parse_target_config({"name": "default", "hicache": {"page_size": 64}}))
        config = hicache_model_config(
            targets[-1], source_prefetch_policy="best_effort", source_target_same_config=False, io_model=self.model()
        )
        self.assertEqual(config["source_prefetch_policy"], "best_effort")
        payload = config["hicache"]
        self.assertEqual(payload["prefetch_policy"], "timeout")
        self.assertEqual(payload["io_cost"]["control_models"]["prefetch"]["fixed_us_per_operation"], 3)

    def model(self, collection=None):
        fields = {
            "kv_bytes_per_token_per_rank": 1,
            "storage_batch_pages": 128,
            "service_models": {},
            "phase_cost": {},
            "resource_lanes": {},
            "control_models": {"prefetch": {"fixed_us_per_operation": 1, "state_check_us_per_operation": 2}},
        }
        if collection is not None:
            fields["control_calibrations"] = control_calibrations(collection)
        return HiCacheIoModel(fields)

    def test_shared_collection_selects_only_declared_coverage(self):
        model = self.model(
            {
                "prefetch_wait": {"wait_complete": "data/wait.json", "best_effort": "data/stop.json"},
                "write_host": {"128": {"write_through": "data/write.json"}},
                "release_host": "data/release.json",
                "write_confirmation": "data/ack.json",
            }
        )

        def projected(page, policy, write):
            target = TargetHiCacheConfig(
                "target", {"page_size": page, "prefetch_policy": policy, "write_policy": write}
            )
            return hicache_model_config(
                target, source_prefetch_policy="timeout", source_target_same_config=False, io_model=model
            )["hicache"]

        first = projected(128, "wait_complete", "write_through")
        expected = {
            "prefetch_wait_calibration": "data/wait.json",
            "write_host_calibration": "data/write.json",
            "release_host_calibration": "data/release.json",
            "write_confirmation_calibration": "data/ack.json",
        }
        self.assertEqual({key: first[key] for key in expected}, expected)
        self.assertNotIn("prefetch_query_calibration", first)
        self.assertEqual(first["prefetch_cpu_calibrations"], ["data/stop.json", "data/wait.json"])
        stop = projected(64, "best_effort", "write_through_selective")
        self.assertEqual(stop["prefetch_wait_calibration"], "data/stop.json")
        self.assertEqual(stop["prefetch_cpu_calibrations"], first["prefetch_cpu_calibrations"])
        self.assertNotIn("write_host_calibration", stop)
        self.assertEqual(stop["release_host_calibration"], first["release_host_calibration"])
        absent = projected(128, "timeout", "write_back")
        self.assertEqual(absent["prefetch_wait_calibration"], "data/wait.json")
        self.assertNotIn("write_host_calibration", absent)

    def test_wait_program_sharing_keeps_policy_and_cost_boundaries(self):
        for policy, counterpart in (("timeout", "wait_complete"), ("wait_complete", "timeout")):
            with self.subTest(policy=policy):
                collection = {"prefetch_wait": {counterpart: "data/shared.json", "best_effort": "data/stop.json"}}
                selected = select_control_calibrations(collection, 32, policy, "write_back")
                self.assertEqual(selected["prefetch_wait_calibration"], "data/shared.json")
                collection["prefetch_wait"][policy] = "data/exact.json"
                self.assertEqual(
                    select_control_calibrations(collection, 32, policy, "write_back")["prefetch_wait_calibration"],
                    "data/exact.json",
                )
        self.assertNotIn(
            "prefetch_wait_calibration",
            select_control_calibrations(
                {"prefetch_wait": {"best_effort": "data/stop.json"}}, 32, "timeout", "write_back"
            ),
        )
        self.assertNotIn(
            "prefetch_wait_calibration",
            select_control_calibrations(
                {"prefetch_wait": {"wait_complete": "data/wait.json"}}, 32, "best_effort", "write_back"
            ),
        )

    def test_query_cost_is_shared_without_changing_target_policy(self):
        model = self.model(
            {
                "prefetch_query": "data/query.json",
                "load_index": "data/load_index.json",
                "load_submission": "data/load_submit.json",
                "layer_wait": "data/layer_wait.json",
            }
        )
        for policy in ("best_effort", "timeout", "wait_complete"):
            target = TargetHiCacheConfig("target", {"page_size": 64, "prefetch_policy": policy})
            projected = hicache_model_config(
                target, source_prefetch_policy="timeout", source_target_same_config=False, io_model=model
            )["hicache"]
            self.assertEqual(projected["prefetch_query_calibration"], "data/query.json")
            self.assertEqual(projected["load_index_calibration"], "data/load_index.json")
            self.assertEqual(projected["load_submission_calibration"], "data/load_submit.json")
            self.assertEqual(projected["layer_wait_calibration"], "data/layer_wait.json")
            self.assertEqual(projected["prefetch_policy"], policy)
            self.assertNotIn("prefetch_wait_calibration", projected)

    def test_write_width_can_share_cost_but_not_lock_policy(self):
        collection = {
            "write_host": {"32": {"write_back": "data/back32.json"}, "128": {"write_through": "data/through128.json"}}
        }
        projected = select_control_calibrations(collection, 128, "timeout", "write_back")
        self.assertEqual(projected["write_host_calibration"], "data/back32.json")
        self.assertNotIn(
            "write_host_calibration", select_control_calibrations(collection, 128, "timeout", "write_through_selective")
        )
        collection["write_host"]["128"]["write_back"] = "data/back128.json"
        self.assertEqual(
            select_control_calibrations(collection, 128, "timeout", "write_back")["write_host_calibration"],
            "data/back128.json",
        )

    def test_targets_cannot_override_shared_templates(self):
        for key in (*CALIBRATION_FIELDS, "control_calibrations"):
            with self.subTest(key=key), self.assertRaises(ValueError):
                parse_target_config({"name": "target", "hicache": {"page_size": 128, key: "data/local.json"}})
