"""Decode allocation observations use Python scalars, never tensor contents."""

from types import ModuleType, SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from trace_sim_probe.probes import decode_allocation as probe
from trace_sim_probe.probes.hicache.common import _cache_scope_key
from trace_sim_probe.writer import _jsonable


class DecodeAllocationTests(unittest.TestCase):
    def test_load_attempt_does_not_inspect_tensors_or_controller(self):
        class Opaque:
            def __bool__(self):
                raise AssertionError("probe must not evaluate tensor truth")

            def __getattr__(self, name):
                raise AssertionError(f"probe must not read {name}")

        controller, indices = Opaque(), Opaque()
        for result in (Opaque(), None):
            for positional in (True, False):
                original = Mock(return_value=result)
                args = (indices, None, 23) if positional else (indices,)
                kwargs = {} if positional else {"node_id": 23}
                writer = Mock()
                writer.now_us.side_effect = [10, 20]
                with patch.object(probe, "get_writer", return_value=writer):
                    self.assertIs(probe._load_attempt(original)(controller, *args, **kwargs), result)
                original.assert_called_once_with(controller, *args, **kwargs)
                self.assertEqual(
                    writer.duration_event.call_args.args,
                    (
                        "runtime.hicache.load_allocation",
                        10,
                        20,
                        "runtime_diagnostic",
                        {"node_id": 23, "status": "returned", "allocated": result is not None},
                    ),
                )

    def test_load_attempt_installation_and_errors(self):
        module = ModuleType("sglang.srt.managers.cache_controller")
        failure = ValueError("allocation failed")
        original = Mock(side_effect=failure)
        module.HiCacheController = type("Controller", (), {"load": original})
        probe.install(module)
        installed = module.HiCacheController.load
        probe.install(module)
        self.assertIs(installed, module.HiCacheController.load)
        writer = Mock()
        writer.now_us.side_effect = [10, 20]
        controller, indices = module.HiCacheController(), object()
        with patch.object(probe, "get_writer", return_value=writer), self.assertRaises(ValueError) as caught:
            controller.load(indices)
        self.assertIs(caught.exception, failure)
        original.assert_called_once_with(controller, indices)
        self.assertEqual(writer.duration_event.call_args.args[4], {"node_id": -1, "status": "raised"})

    def test_capacity_guard_observes_no_eviction_without_inspecting_inputs(self):
        class Cache:
            def __getattr__(self, name):
                raise AssertionError(f"capacity probe must not inspect cache: {name}")

        cache, result = Cache(), object()
        original = Mock(return_value=result)
        writer = Mock()
        writer.now_us.side_effect = [10, 12]
        with patch.object(probe, "get_writer", return_value=writer):
            self.assertIs(probe._capacity_guard(original)(cache, num_tokens=128), result)
        original.assert_called_once_with(cache, num_tokens=128)
        self.assertEqual(
            writer.duration_event.call_args.args,
            ("runtime.hicache.capacity_guard", 10, 12, "runtime_diagnostic", {"status": "returned"}),
        )

    def test_capacity_guard_preserves_errors_and_installation_is_idempotent(self):
        module = ModuleType("sglang.srt.mem_cache.common")
        failure = ValueError("capacity failed")
        original = Mock(side_effect=failure)
        module.evict_from_tree_cache = original
        probe.install(module)
        installed = module.evict_from_tree_cache
        probe.install(module)
        self.assertIs(module.evict_from_tree_cache, installed)
        writer = Mock()
        writer.now_us.side_effect = [10, 20]
        with patch.object(probe, "get_writer", return_value=writer), self.assertRaises(ValueError) as caught:
            module.evict_from_tree_cache(None, 1)
        self.assertIs(caught.exception, failure)
        original.assert_called_once_with(None, 1)
        self.assertEqual(writer.duration_event.call_args.args[4], {"status": "raised"})

    def test_serialization_keeps_every_request_in_a_large_batch(self):
        requests = [{"request_id": str(i), "kv_allocated_len": 128} for i in range(40)]
        self.assertEqual(_jsonable({"requests": requests})["requests"], requests)

    def batch(self):
        class Batch:
            tree_cache = object()
            reqs = [
                SimpleNamespace(rid="a", kv_committed_len=127, kv_allocated_len=127, decode_batch_idx=2),
                SimpleNamespace(rid="b", kv_committed_len=64, kv_allocated_len=65, decode_batch_idx=0),
            ]

            def __getattr__(self, name):
                raise AssertionError(f"probe must not access batch tensor or other state: {name}")

        return Batch()

    def test_entry_values_and_original_result_are_preserved(self):
        batch = self.batch()
        result = object()
        calls = []

        def allocate(actual, token_per_req):
            calls.append((actual, token_per_req))
            actual.reqs[0].kv_allocated_len += token_per_req
            return result

        writer = Mock()
        writer.now_us.side_effect = [10, 30]
        with patch.object(probe, "get_writer", return_value=writer):
            self.assertIs(probe._allocation(allocate)(batch, token_per_req=1), result)
        self.assertEqual(calls, [(batch, 1)])
        event = writer.duration_event.call_args.args
        self.assertEqual(event[:4], ("runtime.hicache.decode_allocation", 10, 30, "runtime_diagnostic"))
        self.assertEqual(event[4]["cache_scope"], _cache_scope_key(batch.tree_cache))
        self.assertEqual(event[4]["requests"][0]["kv_allocated_len"], 127)
        self.assertEqual(event[4]["requests"][1]["request_id"], "b")
        self.assertEqual(event[4]["token_per_req"], 1)
        self.assertEqual(event[4]["status"], "returned")
        self.assertNotIn("fact", event[4])

    def test_failure_is_not_reported_as_a_successful_allocation(self):
        failure = ValueError("allocation failed")
        original = Mock(side_effect=failure)
        writer = Mock()
        writer.now_us.side_effect = [10, 20]
        with patch.object(probe, "get_writer", return_value=writer), self.assertRaises(ValueError) as caught:
            probe._allocation(original)(self.batch(), 1)
        self.assertIs(caught.exception, failure)
        original.assert_called_once()
        self.assertEqual(writer.duration_event.call_args.args[4]["status"], "raised")

    def test_alias_installation_never_double_wraps_a_call(self):
        for import_before in (False, True):
            definition, caller = (ModuleType(name) for name in probe.TARGET_MODULES[:2])
            definition.alloc_for_decode = Mock(return_value=object())
            original = definition.alloc_for_decode
            if import_before:
                caller.alloc_for_decode = original
            probe.install(definition)
            if not import_before:
                caller.alloc_for_decode = definition.alloc_for_decode
            probe.install(caller)
            installed = caller.alloc_for_decode
            probe.install(caller)
            self.assertIs(installed, caller.alloc_for_decode)
            writer = Mock()
            writer.now_us.return_value = 10
            with patch.object(probe, "get_writer", return_value=writer):
                caller.alloc_for_decode(self.batch(), 1)
            original.assert_called_once()
            writer.duration_event.assert_called_once()


if __name__ == "__main__":
    unittest.main()
