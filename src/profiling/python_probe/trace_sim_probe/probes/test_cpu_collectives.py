"""Device-free checks of collective metadata and transparent call behavior."""
import types
import unittest
from unittest.mock import Mock, patch

from trace_sim_probe.probes import cpu_collectives as probe
from trace_sim_probe.writer import _jsonable


class CollectiveCheck(unittest.TestCase):
    def setUp(self):
        groups_patch = patch.dict(probe._GROUPS, clear=True)
        groups_patch.start()
        self.addCleanup(groups_patch.stop)
        self.sequence = 7
        self.group = Mock()
        self.group._get_sequence_number_for_group.side_effect = lambda: self.sequence
        self.tensor = types.SimpleNamespace(device=types.SimpleNamespace(type="cpu"), dtype="int64", numel=lambda: 2)
        self.work = Mock()
        self.module = types.ModuleType("torch.distributed.distributed_c10d")
        self.module._rank_not_in_group = lambda group: False
        self.module.get_backend = lambda group: "gloo"
        self.module._get_default_group = lambda: self.group
        self.module._get_process_group_name = lambda group: "group"
        self.module.get_process_group_ranks = lambda group: [2, 5]
        self.module.get_rank = lambda: 5

        def broadcast(tensor, src=None, group=None, async_op=False, group_src=None):
            if src == -1:
                raise ValueError("invalid root")
            self.sequence += 1
            return self.work if async_op else None

        def all_reduce(tensor, op="sum", group=None, async_op=False):
            if op != "coalesced":
                self.sequence += 1
            return self.work if async_op else None

        self.module.broadcast, self.module.all_reduce = broadcast, all_reduce
        self.writer = Mock()
        self.writer.now_us.side_effect = range(100, 200)
        self.writer_patch = patch.object(probe, "get_writer", return_value=self.writer)
        self.writer_patch.start()
        self.addCleanup(self.writer_patch.stop)
        with patch.dict("sys.modules", {self.module.__name__: self.module}):
            probe.install(self.module)
            wrapped = self.module.broadcast
            probe.install(self.module)
            self.assertIs(wrapped, self.module.broadcast)

    def records(self):
        return [call.args[4] for call in self.writer.duration_event.call_args_list]

    def test_sync_and_async_keep_sequence_and_return(self):
        self.assertIsNone(self.module.broadcast(self.tensor, 2))
        self.assertIs(self.module.all_reduce(self.tensor, async_op=True), self.work)
        self.work.wait.assert_not_called()
        first, second = self.records()
        self.assertEqual([r["collective_index"] for r in self.records()], [0, 1])
        self.assertEqual(first["observation_start_sequence"], 7)
        self.assertEqual((first["sequence_before"], first["sequence_after"]), (7, 8))
        self.assertEqual((second["sequence_before"], second["sequence_after"]), (8, 9))
        self.assertEqual(first["members"], [2, 5])
        self.assertEqual(first["src"], 2)
        self.assertTrue(second["async_op"])
        self.assertEqual(second["status"], "returned")
        self.assertNotIn("tensor", first)

    def test_exception_and_coalescing_do_not_invent_completion(self):
        with self.assertRaisesRegex(ValueError, "invalid root"):
            self.module.broadcast(self.tensor, src=-1)
        self.module.all_reduce(self.tensor, op="coalesced")
        first, second = self.records()
        self.assertEqual(first["status"], "raised")
        self.assertEqual((first["sequence_before"], first["sequence_after"]), (7, 7))
        self.assertEqual((second["sequence_before"], second["sequence_after"]), (7, 7))
        self.module.broadcast(self.tensor, src=2)
        self.assertEqual([r["collective_index"] for r in self.records()], [0, 1, 2])

    def test_call_order_is_shared_across_entrypoints_and_ignores_native_jumps(self):
        alias = types.ModuleType("torch.distributed")
        alias.broadcast = self.module.broadcast
        alias.all_reduce = self.module.all_reduce
        with patch.dict("sys.modules", {self.module.__name__: self.module}):
            probe.install(alias)
        self.module.broadcast(self.tensor, src=2)
        self.sequence += 6  # Unobserved P2P/barrier steps are not tensor collectives.
        alias.all_reduce(self.tensor)
        alias.broadcast(self.tensor, src=2)
        self.assertEqual([r["collective_index"] for r in self.records()], [0, 1, 2])
        self.assertEqual([r["sequence_before"] for r in self.records()], [7, 14, 15])
        self.assertTrue(all(r["observation_start_sequence"] == 7 for r in self.records()))

    def test_groups_have_independent_call_order(self):
        other = Mock()
        other._get_sequence_number_for_group.side_effect = lambda: self.sequence
        self.module._get_process_group_name = lambda group: "default" if group is self.group else "other"
        self.module.broadcast(self.tensor, src=2)
        self.module.all_reduce(self.tensor, group=other)
        self.module.all_reduce(self.tensor)
        self.module.broadcast(self.tensor, src=2, group=other)
        self.assertEqual([(r["group"], r["collective_index"]) for r in self.records()],
                         [("default", 0), ("other", 0), ("default", 1), ("other", 1)])

    def test_device_and_other_backend_are_unwrapped(self):
        self.tensor.device.type = "npu"
        self.module.broadcast(self.tensor, src=2)
        self.tensor.device.type = "cpu"
        self.module.get_backend = lambda group: "other"
        self.module.broadcast(self.tensor, src=2)
        self.assertEqual(self.sequence, 9)
        self.writer.duration_event.assert_not_called()

    def test_nonmember_is_not_inspected(self):
        self.module._rank_not_in_group = lambda group: True
        self.module.broadcast(self.tensor, src=2)
        self.writer.duration_event.assert_not_called()
        self.group._get_sequence_number_for_group.assert_not_called()

    def test_group_identity_is_not_truncated_by_writer(self):
        self.module.get_process_group_ranks = lambda group: list(range(64))
        self.module.broadcast(self.tensor, src=2)
        record = _jsonable(self.records()[0])
        self.assertEqual(record["members"], list(range(64)))

    def test_nested_compatibility_wrapper_records_one_native_call(self):
        inner = self.module.broadcast

        def compatibility(tensor, src=None, group=None, async_op=False, group_src=None):
            return inner(tensor, src, group, async_op, group_src)

        outer = probe._wrap(compatibility, "broadcast", self.module)
        self.assertIs(outer(self.tensor, src=2, async_op=True), self.work)
        self.assertEqual(len(self.records()), 1)
        self.assertEqual(self.records()[0]["collective_index"], 0)
        self.assertEqual((self.records()[0]["sequence_before"], self.records()[0]["sequence_after"]), (7, 8))
        with self.assertRaisesRegex(ValueError, "invalid root"):
            outer(self.tensor, src=-1)
        self.assertEqual(len(self.records()), 2)
        self.assertEqual(self.records()[-1]["status"], "raised")
        self.assertEqual(self.records()[-1]["collective_index"], 1)
        self.assertFalse(probe._CALLS.get())
        self.work.wait.assert_not_called()

    def test_nested_different_operation_is_not_silenced(self):
        inner = self.module.broadcast

        def combined(tensor, src=None, group=None, async_op=False, group_src=None):
            self.module.all_reduce(tensor, group=group)
            return inner(tensor, src, group, async_op, group_src)

        probe._wrap(combined, "broadcast", self.module)(self.tensor, src=2)
        first, second = self.records()
        self.assertEqual(first["operation"], "all_reduce")
        self.assertEqual(second["sequence_after"] - second["sequence_before"], 2)
        self.assertFalse(probe._CALLS.get())
