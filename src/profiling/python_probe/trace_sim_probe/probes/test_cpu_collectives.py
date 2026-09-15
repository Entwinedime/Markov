"""Device-free checks of collective metadata and transparent call behavior."""
import types
import unittest
from unittest.mock import Mock, patch

from trace_sim_probe.probes import cpu_collectives as probe
from trace_sim_probe.writer import _jsonable


class CollectiveCheck(unittest.TestCase):
    def setUp(self):
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
