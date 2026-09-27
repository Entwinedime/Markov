"""Device-free checks of collective metadata and transparent call behavior."""

import types
import unittest
from concurrent.futures import ThreadPoolExecutor
from functools import partial, wraps
from unittest.mock import Mock, patch

from trace_sim_probe.patching import PATCH_MARKER
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

    def install_owner(self, module_name: str, class_name: str, owner: type) -> None:
        module = types.ModuleType(module_name)
        setattr(module, class_name, owner)
        probe.install(module)
        installed = dict(vars(owner))

        probe.install(module)
        self.assertEqual(installed, dict(vars(owner)))

    def records(self):
        return [
            call.args[4]
            for call in self.writer.duration_event.call_args_list
            if call.args[0] == "runtime.cpu_collective"
        ]

    def test_device_restore_observes_interval_without_reading_values(self):
        class Opaque:
            def __getitem__(self, key):
                raise AssertionError("probe must not slice tensor")

            def __len__(self):
                raise AssertionError("probe must not inspect tensor")

        failure = ValueError("restore failed")
        result, value = object(), Opaque()
        calls = []

        class Cache:
            def _restore_device_value(self, node, value, prefix_len):
                calls.append((node, value, prefix_len))
                if prefix_len < 0:
                    raise failure
                return result

            def insert(self, params):
                return self._restore_device_value(params.node, value, params.prefix_len)

        self.install_owner("sglang.srt.mem_cache.hiradix_cache", "HiRadixCache", Cache)
        owner = Cache()
        parent = types.SimpleNamespace(key=range(2048), parent=None)
        node = types.SimpleNamespace(id=9, key=range(128), parent=parent)
        self.assertIs(owner._restore_device_value(node, value, 128), result)
        self.assertEqual(self.writer.duration_event.call_count, 0)
        for prefix_len in (128, -1):
            self.writer.duration_event.reset_mock()
            params = types.SimpleNamespace(node=node, key=range(2176), chunked=False, prefix_len=prefix_len)
            if prefix_len > 0:
                self.assertIs(owner.insert(params), result)
            else:
                with self.assertRaises(ValueError) as caught:
                    owner.insert(params)
                self.assertIs(caught.exception, failure)
            restore, insert = [call.args for call in self.writer.duration_event.call_args_list]
            self.assertEqual(restore[0], "runtime.hicache.device_restore")
            self.assertEqual(
                restore[4],
                dict(
                    node_id=9,
                    chunked=False,
                    path_begin_tokens=2048,
                    path_end_tokens=2176,
                    status="returned" if prefix_len > 0 else "raised",
                ),
            )
            self.assertLess(insert[1], restore[1])
            self.assertLess(restore[2], insert[2])
            self.assertIsNone(probe._RADIX_INSERT.get())
            self.assertEqual(calls[-1], (node, value, prefix_len))

    def test_write_policy_check_preserves_entry_state_and_behavior(self):
        class HostValue:
            def __bool__(self):
                raise AssertionError("probe must not inspect tensor values")

        sentinel = object()
        calls = []

        class Cache:
            def _inc_hit_count(self, node, chunked=False):
                calls.append((node, chunked))
                node.hit_count = 99
                node.host_value = HostValue()
                return sentinel

        self.install_owner("sglang.srt.mem_cache.hiradix_cache", "HiRadixCache", Cache)
        owner = Cache()
        owner.write_through_threshold = 2
        owner.cache_controller = types.SimpleNamespace(write_policy="write_through_selective")
        for backed in (False, True):
            with self.subTest(backed=backed):
                node = types.SimpleNamespace(
                    id=7, hit_count=1, key=range(16), parent=None, host_value=HostValue() if backed else None
                )
                self.assertIs(owner._inc_hit_count(node=node, chunked=backed), sentinel)
                self.assertEqual(calls[-1], (node, backed))
                call = self.writer.duration_event.call_args.args
                self.assertEqual(call[0], "runtime.hicache.write_policy_check")
                self.assertEqual(
                    call[4],
                    dict(
                        node_id=7,
                        hit_count=1,
                        backuped=backed,
                        write_policy="write_through_selective",
                        chunked=backed,
                        write_through_threshold=2,
                        status="returned",
                        path_begin_tokens=0,
                        path_end_tokens=16,
                    ),
                )
                self.assertLess(call[1], call[2])

    def test_insert_publication_exists_without_write_policy_call(self):
        calls = []
        result = object()

        class Cache:
            def _record_store_event(self, node, medium=None):
                calls.append((node.id, medium))
                return result

            def insert(self, params):
                self._record_store_event(params.node, medium="CPU")
                self._record_store_event(params.node)
                return result  # write_back never calls _inc_hit_count for this leaf

        self.install_owner("sglang.srt.mem_cache.hiradix_cache", "HiRadixCache", Cache)
        owner = Cache()
        node = types.SimpleNamespace(id=17, key=range(16), parent=None)
        self.assertIs(owner._record_store_event(node), result)
        self.assertEqual(self.writer.duration_event.call_count, 0)
        for chunked in (False, True):
            self.writer.duration_event.reset_mock()
            params = types.SimpleNamespace(chunked=chunked, node=node, key=range(16))
            self.assertIs(owner.insert(params=params), result)
            events = [call.args for call in self.writer.duration_event.call_args_list]
            self.assertEqual(
                [event[0] for event in events], ["runtime.hicache.node_publish", "runtime.hicache.radix_insert"]
            )
            publish, insert = events
            self.assertLess(insert[1], publish[1])
            self.assertLess(publish[2], insert[2])
            self.assertEqual(
                publish[4],
                dict(node_id=17, chunked=chunked, status="returned", path_begin_tokens=0, path_end_tokens=16),
            )
            self.assertEqual(insert[4]["path_tokens"], 16)
            self.assertIsNone(probe._RADIX_INSERT.get())
        self.assertEqual(calls, [(17, None), (17, "CPU"), (17, None), (17, "CPU"), (17, None)])

    def test_insert_publication_failure_restores_context(self):
        failure = ValueError("publication failed")

        def publish(owner, node, medium=None):
            raise failure

        observed_publish = probe._node_publish(publish)

        def insert(owner, params):
            return observed_publish(owner, params.node)

        params = types.SimpleNamespace(
            chunked=False, key=range(16), node=types.SimpleNamespace(id=1, key=range(16), parent=None)
        )
        with self.assertRaises(ValueError) as caught:
            probe._radix_insert(insert)(object(), params)
        self.assertIs(caught.exception, failure)
        self.assertIsNone(probe._RADIX_INSERT.get())
        events = [call.args for call in self.writer.duration_event.call_args_list]
        self.assertEqual(len(events), 2)
        self.assertTrue(all(event[4]["status"] == "raised" for event in events))

    def test_prefetch_progress_records_entry_branch_without_reading_tensor(self):
        class HostIndices:
            def __bool__(self):
                raise AssertionError("probe must not inspect tensor values")

        operation = types.SimpleNamespace(host_indices=None)
        owner = types.SimpleNamespace(ongoing_prefetch={})

        def progress(cache, req_id):
            # Match completion removing an active operation before returning.
            cache.ongoing_prefetch.pop(req_id, None)
            return True

        observed = probe._prefetch_progress(progress)
        for branch, entry in (
            ("no_operation", None),
            ("host_not_allocated", (None, None, None, operation)),
            ("active", (None, None, None, types.SimpleNamespace(host_indices=HostIndices()))),
        ):
            with self.subTest(branch=branch):
                if entry is not None:
                    owner.ongoing_prefetch["q"] = entry
                self.assertTrue(observed(owner, req_id="q"))
                fields = self.writer.duration_event.call_args.args[4]
                self.assertEqual(
                    fields, {"request_id": "q", "entry_branch": branch, "status": "returned", "progress_ready": True}
                )

        def failed(cache, req_id):
            raise ValueError("progress failed")

        with self.assertRaisesRegex(ValueError, "progress failed"):
            probe._prefetch_progress(failed)(owner, "q")
        fields = self.writer.duration_event.call_args.args[4]
        self.assertEqual(fields["status"], "raised")
        self.assertNotIn("progress_ready", fields)

    def test_host_load_check_preserves_false_true_retries_and_exceptions(self):
        class Req:
            rid = "request"
            result = False

            def needs_host_load_back(self):
                if isinstance(self.result, Exception):
                    raise self.result
                return self.result

            def __getattr__(self, name):
                raise AssertionError(f"probe must not inspect request state: {name}")

        self.install_owner("sglang.srt.managers.schedule_batch", "Req", Req)
        req = Req()
        for value in (False, True, False):
            req.result = value
            self.assertIs(req.needs_host_load_back(), value)
            name, _, _, category, fields = self.writer.duration_event.call_args.args
            self.assertEqual((name, category), ("runtime.hicache.host_load_check", "runtime_diagnostic"))
            self.assertEqual(fields, {"request_id": "request", "status": "returned", "needed": value})
        req.result = ValueError("original failure")
        with self.assertRaisesRegex(ValueError, "original failure"):
            req.needs_host_load_back()
        self.assertEqual(self.writer.duration_event.call_count, 4)
        self.assertEqual(self.writer.duration_event.call_args.args[4], {"request_id": "request", "status": "raised"})

    def test_device_release_only_observes_existing_return_value(self):
        class Node:
            def __getattr__(self, name):
                raise AssertionError(f"release probe must not inspect node: {name}")

        node = Node()
        for stage in ("device_release_backup", "device_release_regular"):
            original = Mock(return_value=128)
            self.assertEqual(probe._device_release(original, stage)(object(), node=node), 128)
            original.assert_called_once()
            self.assertIs(original.call_args.kwargs["node"], node)
            event = self.writer.duration_event.call_args.args
            self.assertEqual(event[0], "runtime.hicache." + stage)
            self.assertEqual(event[4], {"status": "returned", "released_tokens": 128})

    def test_allocator_boundary_is_scoped_metadata_only_and_preserves_failure(self):
        class Indices:
            def numel(self):
                return 1152

            def __getattr__(self, name):
                raise AssertionError(f"probe must not read tensor content: {name}")

        class Allocator:
            page_size, is_not_in_free_group, need_sort = 64, True, False

            def free(self, free_index):
                return "unchanged"

        self.install_owner("sglang.srt.mem_cache.allocator.paged", "PagedTokenToKVPoolAllocator", Allocator)
        allocator, indices = Allocator(), Indices()
        self.assertEqual(allocator.free(free_index=indices), "unchanged")
        self.writer.duration_event.assert_not_called()

        def release(*args):
            self.assertEqual(allocator.free(indices), "unchanged")
            return 1152

        for immediate in (True, False):
            allocator.is_not_in_free_group = immediate
            self.writer.duration_event.reset_mock()
            wrapped = probe._device_release(release, "device_release_regular")
            self.assertEqual(wrapped(), 1152)
            events = [call.args for call in self.writer.duration_event.call_args_list]
            self.assertEqual(
                [event[0] for event in events],
                ["runtime.hicache.allocator_free", "runtime.hicache.device_release_regular"],
            )
            self.assertEqual(
                events[0][4],
                dict(
                    status="returned",
                    release="device_release_regular",
                    tokens=1152,
                    page_size=64,
                    immediate=immediate,
                    need_sort=False,
                ),
            )
            self.assertIsNone(probe._DEVICE_RELEASE.get())
        failure = ValueError("allocator failed")

        def fail(allocator, free_index):
            raise failure

        wrapped = probe._device_release(
            lambda: probe._allocator_free(fail)(allocator, indices), "device_release_backup"
        )
        with self.assertRaises(ValueError) as caught:
            wrapped()
        self.assertIs(caught.exception, failure)
        self.assertIsNone(probe._DEVICE_RELEASE.get())
        self.assertEqual(self.writer.duration_event.call_args_list[-2].args[4]["status"], "raised")

    def test_device_release_preserves_exception_and_is_installed_once(self):
        failure = ValueError("release failed")

        class Cache:
            def _evict_backuped(self, node):
                raise failure

            def _evict_regular(self, node):
                return 0

        self.install_owner("sglang.srt.mem_cache.hiradix_cache", "HiRadixCache", Cache)
        with self.assertRaises(ValueError) as caught:
            Cache()._evict_backuped(None)
        self.assertIs(caught.exception, failure)
        self.assertEqual(self.writer.duration_event.call_args.args[4], {"status": "raised"})
        self.assertEqual(Cache()._evict_regular(None), 0)
        self.assertEqual(self.writer.duration_event.call_args.args[4], {"status": "returned", "released_tokens": 0})

    def test_completion_checks_only_observe_lengths_and_preserve_calls(self):
        collective, tensor = self.module.all_reduce, self.tensor
        result = object()

        class Queue(list):
            def __iter__(self):
                raise AssertionError("completion observation must not scan ACK entries")

        class Cache:
            def __init__(self):
                self.cache_controller = types.SimpleNamespace(
                    ack_load_queue=Queue([object(), object()]), ack_write_queue=Queue([object()])
                )
                self.ongoing_load_back = {1: object(), 2: object(), 3: object()}
                self.ongoing_write_through = {4: object()}

            def loading_check(self):
                collective(tensor, op="min")
                self.cache_controller.ack_load_queue.pop(0)
                self.ongoing_load_back.pop(1)
                self.ongoing_load_back.pop(2)
                return result

            def writing_check(self, write_back=False):
                if not write_back:
                    return result
                self.cache_controller.ack_write_queue.clear()
                self.ongoing_write_through.clear()
                raise ValueError("original failure")

        self.install_owner("sglang.srt.mem_cache.hiradix_cache", "HiRadixCache", Cache)
        cache = Cache()
        self.assertIs(cache.loading_check(), result)
        event, begin, end, _, fields = self.writer.duration_event.call_args.args
        self.assertEqual((event, begin, end), ("runtime.hicache.load_completion", 100, 103))
        self.assertEqual(
            fields,
            {
                "batches_before": 2,
                "batches_after": 1,
                "operations_before": 3,
                "operations_after": 1,
                "blocking": False,
                "status": "returned",
            },
        )
        self.assertEqual(self.records()[0]["role"], "load_completion_check")
        self.assertIs(cache.writing_check(write_back=False), result)
        self.assertFalse(self.writer.duration_event.call_args.args[4]["blocking"])
        with self.assertRaisesRegex(ValueError, "original failure"):
            cache.writing_check(True)
        event, _, _, _, fields = self.writer.duration_event.call_args.args
        self.assertEqual(event, "runtime.hicache.write_completion")
        self.assertEqual(
            fields,
            {
                "batches_before": 1,
                "batches_after": 0,
                "operations_before": 1,
                "operations_after": 0,
                "blocking": True,
                "status": "raised",
            },
        )
        self.assertEqual(probe._CONTEXT.get(), {})

    def test_prefetch_stop_preserves_existing_scalar_return_and_identity(self):
        result = (128, ["key"])

        class Controller:
            def terminate_prefetch(self, operation):
                operation.mark_terminate()
                return result

        self.install_owner("sglang.srt.managers.cache_controller", "HiCacheController", Controller)
        operation = types.SimpleNamespace(request_id="request-1", mark_terminate=Mock())
        self.assertIs(Controller().terminate_prefetch(operation=operation), result)
        operation.mark_terminate.assert_called_once_with()
        event, begin, end, category, fields = self.writer.duration_event.call_args.args
        self.assertEqual(
            (event, begin, end, category), ("runtime.hicache.prefetch_stop", 100, 101, "runtime_diagnostic")
        )
        self.assertEqual(fields, {"request_id": "request-1", "status": "returned", "completed_tokens": 128})

    def test_query_release_and_drain_use_existing_scalars_only(self):
        query_result = (object(), 64)  # Never inspect the returned hash collection.

        class Indices:
            def numel(self):
                return 128

            def __iter__(self):
                raise AssertionError("release observation must not read tensor contents")

        class Controller:
            page_size = 64
            prefetch_sync_groups = (object(), object())
            mem_pool_host = types.SimpleNamespace(page_size=64)

            def _storage_hit_query(self, operation):
                return query_result

            def append_host_mem_release(self, host_indices):
                return host_indices

        class Cache:
            tp_world_size = 2

            def _drain_storage_control_queues_impl(self, n_revoke, n_backup, n_release, log_metrics):
                return n_revoke, n_backup, n_release, log_metrics

        self.install_owner("sglang.srt.managers.cache_controller", "HiCacheController", Controller)
        self.install_owner("sglang.srt.mem_cache.hiradix_cache", "HiRadixCache", Cache)
        operation = types.SimpleNamespace(request_id="query-request", id=42)
        self.assertIs(Controller()._storage_hit_query(operation=operation), query_result)
        indices = Indices()
        self.assertIs(Controller().append_host_mem_release(host_indices=indices), indices)
        self.assertEqual(
            Cache()._drain_storage_control_queues_impl(n_revoke=1, n_backup=2, n_release=3, log_metrics=True),
            (1, 2, 3, True),
        )
        self.assertEqual(Cache()._drain_storage_control_queues_impl(None, None, None, False), (None, None, None, False))
        rows = [call.args for call in self.writer.duration_event.call_args_list]
        self.assertEqual(
            [row[0] for row in rows],
            [
                "runtime.hicache.prefetch_query",
                "runtime.hicache.host_release",
                "runtime.hicache.storage_drain",
                "runtime.hicache.storage_drain",
            ],
        )
        self.assertEqual(
            rows[0][4],
            {
                "request_id": "query-request",
                "operation_id": 42,
                "sync_group_count": 2,
                "status": "returned",
                "local_hit_tokens": 64,
                "page_size": 64,
            },
        )
        self.assertEqual(rows[1][4], {"token_count": 128, "page_size": 64, "status": "returned"})
        self.assertEqual(
            rows[2][4], {"n_revoke": 1, "n_backup": 2, "n_release": 3, "tp_world_size": 2, "status": "returned"}
        )
        self.assertIsNone(rows[3][4]["n_release"], "shutdown/unbounded drain cannot be reported as zero work")

    def test_failed_operations_preserve_exceptions_without_inventing_results(self):
        owner = types.SimpleNamespace(prefetch_sync_groups=())
        operation = types.SimpleNamespace(request_id="request")
        cases = (
            (partial(probe._prefetch_interval, stage="prefetch_query"), (owner, operation)),
            (probe._prefetch_publication, (operation, 64)),
        )
        for wrap, arguments in cases:
            with self.subTest(wrapper=wrap):
                failure = ValueError("original operation failed")
                original = Mock(side_effect=failure)
                self.writer.duration_event.reset_mock()

                with self.assertRaises(ValueError) as caught:
                    wrap(original)(*arguments)

                self.assertIs(caught.exception, failure)
                original.assert_called_once()
                self.assertEqual(self.writer.duration_event.call_count, 1)
                fields = self.writer.duration_event.call_args.args[4]
                self.assertEqual(fields["status"], "raised")
                self.assertTrue({"local_hit_tokens", "accepted", "completed_tokens"}.isdisjoint(fields))

    def test_submission_and_frontend_release_context_are_transparent(self):
        submitted = types.SimpleNamespace(id=41)
        indices = types.SimpleNamespace(numel=lambda: 128)

        class Controller:
            page_size = 64
            mem_pool_host = types.SimpleNamespace(page_size=64)

            def prefetch(self, request_id, host_indices, new_input_tokens, last_hash=None, prefix_keys=None):
                if last_hash == "fail":
                    raise ValueError("submit failed")
                return submitted

            def append_host_mem_release(self, host_indices):
                return host_indices

        controller = Controller()

        class Cache:
            def check_prefetch_progress(self, req_id):
                return controller.append_host_mem_release(indices)

            def release_aborted_request(self, rid):
                controller.append_host_mem_release(indices)
                raise ValueError("abort failed")

        self.install_owner("sglang.srt.managers.cache_controller", "HiCacheController", Controller)
        self.install_owner("sglang.srt.mem_cache.hiradix_cache", "HiRadixCache", Cache)
        self.assertIs(controller.prefetch(request_id="q", host_indices=indices, new_input_tokens=[1, 2]), submitted)
        with self.assertRaisesRegex(ValueError, "submit failed"):
            controller.prefetch("q", indices, [1, 2], last_hash="fail")
        self.assertIs(Cache().check_prefetch_progress(req_id="complete"), indices)
        with self.assertRaisesRegex(ValueError, "abort failed"):
            Cache().release_aborted_request(rid="aborted")
        self.assertIs(controller.append_host_mem_release(indices), indices)
        rows = [
            call.args[4]
            for call in self.writer.duration_event.call_args_list
            if call.args[0] != "runtime.hicache.prefetch_progress"
        ]
        self.assertEqual(
            rows[0],
            {
                "request_id": "q",
                "token_count": 2,
                "allocated_tokens": 128,
                "page_size": 64,
                "status": "returned",
                "operation_id": 41,
            },
        )
        self.assertEqual(rows[1]["status"], "raised")
        self.assertNotIn("operation_id", rows[1])
        self.assertEqual((rows[2]["role"], rows[2]["request_id"]), ("prefetch_completion", "complete"))
        self.assertEqual((rows[3]["role"], rows[3]["request_id"]), ("prefetch_abort", "aborted"))
        self.assertNotIn("request_id", rows[4], "completed/failed scopes must not leak into later releases")
        self.assertNotIn("role", rows[4])

    def test_prefetch_boundaries_record_false_and_exception_without_new_work(self):
        check = probe._prefetch_interval(lambda owner, operation: False, "prefetch_check")
        operation = types.SimpleNamespace(request_id="request-2")
        self.assertFalse(check(None, operation))
        self.assertEqual(self.writer.duration_event.call_args.args[4]["can_terminate"], False)

        def failed(owner, operation):
            raise ValueError("test failure")

        with self.assertRaisesRegex(ValueError, "test failure"):
            probe._prefetch_interval(failed, "prefetch_stop")(None, operation)
        self.assertEqual(self.writer.duration_event.call_args.args[4], {"request_id": "request-2", "status": "raised"})

    def test_publication_preserves_accepted_and_cancelled_increments(self):
        class Operation:
            request_id = "request-3"
            completed_tokens = 0
            terminated = False

            def increment(self, num_tokens):
                if self.terminated:
                    return False
                self.completed_tokens += num_tokens
                return True

            @property
            def host_indices(self):
                raise AssertionError("publication observation must not inspect a tensor")

        self.install_owner("sglang.srt.managers.cache_controller", "PrefetchOperation", Operation)
        operation = Operation()
        self.assertTrue(operation.increment(num_tokens=64))
        self.assertTrue(operation.increment(64))
        operation.terminated = True
        self.assertFalse(operation.increment(64))
        self.assertEqual(operation.completed_tokens, 128)
        rows = [call.args for call in self.writer.duration_event.call_args_list]
        self.assertTrue(
            all(row[0] == "runtime.hicache.prefetch_publish" and row[3] == "runtime_diagnostic" for row in rows)
        )
        self.assertEqual([row[4]["completed_tokens"] for row in rows], [64, 128, 128])
        self.assertEqual([row[4]["accepted"] for row in rows], [True, True, False])
        self.assertEqual(
            rows[-1][4],
            {
                "request_id": "request-3",
                "num_tokens": 64,
                "status": "returned",
                "accepted": False,
                "completed_tokens": 128,
            },
        )

    def test_backend_read_records_count_without_touching_payload(self):
        result = object()
        keys = [object(), object()]
        destinations = object()

        class File:
            def batch_get(owner, keys, target_locations, target_sizes=None):
                self.assertIs(keys, expected_keys)
                self.assertIs(target_locations, destinations)
                if target_sizes == "fail":
                    raise ValueError("read failed")
                return result

        expected_keys = keys
        self.assertIn("sglang.srt.mem_cache.hicache_storage", probe.TARGET_MODULES)
        self.install_owner("sglang.srt.mem_cache.hicache_storage", "HiCacheFile", File)
        self.assertIs(File().batch_get(keys=keys, target_locations=destinations), result)
        event, begin, end, category, fields = self.writer.duration_event.call_args.args
        self.assertEqual((event, category), ("runtime.hicache.prefetch_read", "runtime_diagnostic"))
        self.assertEqual(fields, {"page_count": 2, "status": "returned"})
        with self.assertRaisesRegex(ValueError, "read failed"):
            File().batch_get(keys, destinations, target_sizes="fail")
        self.assertEqual(self.writer.duration_event.call_args.args[4], {"page_count": 2, "status": "raised"})

    def test_sync_and_async_keep_sequence_and_return(self):
        self.module.get_process_group_ranks = lambda group: list(range(64))
        self.assertIsNone(self.module.broadcast(self.tensor, 2))
        self.assertIs(self.module.all_reduce(self.tensor, async_op=True), self.work)
        self.work.wait.assert_not_called()
        first, second = self.records()
        self.assertEqual([r["collective_index"] for r in self.records()], [0, 1])
        self.assertEqual(first["observation_start_sequence"], 7)
        self.assertEqual((first["sequence_before"], first["sequence_after"]), (7, 8))
        self.assertEqual((second["sequence_before"], second["sequence_after"]), (8, 9))
        self.assertEqual(_jsonable(first)["members"], list(range(64)))
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
        self.assertEqual(
            [(r["group"], r["collective_index"]) for r in self.records()],
            [("default", 0), ("other", 0), ("default", 1), ("other", 1)],
        )

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

    def test_framework_roles_preserve_nested_phase_and_request(self):
        collective, tensor = self.module.all_reduce, self.tensor

        class Cache:
            cache_controller = types.SimpleNamespace(ack_write_queue=[])
            ongoing_write_through = {}

            def writing_check(self):
                return collective(tensor)

            def can_terminate_prefetch(self, operation):
                collective(tensor, op="max")
                return True

            def check_prefetch_progress(self, req_id):
                self.can_terminate_prefetch(None)
                collective(tensor, op="min")
                return True

        cache = Cache()

        class Scheduler:
            def get_new_batch_prefill(self):
                return cache.check_prefetch_progress(req_id="request-1")

            def update_running_batch(self):
                return cache.writing_check()

        # Context installation must work before distributed modules are loaded.
        with patch.dict("sys.modules", {self.module.__name__: None}):
            self.install_owner("sglang.srt.mem_cache.hiradix_cache", "HiRadixCache", Cache)
            self.install_owner("sglang.srt.managers.scheduler", "Scheduler", Scheduler)
        self.assertTrue(Scheduler().get_new_batch_prefill())
        Scheduler().update_running_batch()
        state, completion, decode = self.records()
        self.assertEqual(
            [row["role"] for row in self.records()],
            ["prefetch_state_check", "prefetch_completion", "write_completion_check"],
        )
        self.assertEqual(state["request_id"], completion["request_id"])
        self.assertEqual(completion["request_id"], "request-1")
        self.assertEqual(state["scheduler_phase"], "prefill_selection")
        self.assertEqual(decode["scheduler_phase"], "decode_update")
        self.assertNotIn("request_id", decode)
        self.assertEqual(probe._CONTEXT.get(), {})

    def test_context_does_not_leak_after_exception_or_into_another_thread(self):
        def failed():
            with ThreadPoolExecutor(max_workers=1) as executor:
                self.assertEqual(executor.submit(probe._CONTEXT.get).result(), {})
            self.module.broadcast(self.tensor, src=-1)

        with self.assertRaisesRegex(ValueError, "invalid root"):
            probe._scope(failed, "role", "write_completion_check")()
        self.module.broadcast(self.tensor, src=2)
        self.assertEqual(self.records()[0]["role"], "write_completion_check")
        self.assertNotIn("role", self.records()[1])
        self.assertEqual(probe._CONTEXT.get(), {})

    def test_context_and_semantic_wrappers_coexist_in_either_order(self):
        def semantic(original):
            @wraps(original)
            def wrapped(*args, **kwargs):
                return original(*args, **kwargs)

            setattr(wrapped, PATCH_MARKER, True)
            return wrapped

        for semantic_first in (True, False):

            class Cache:
                cache_controller = types.SimpleNamespace(ack_write_queue=[])
                ongoing_write_through = {}

            Cache.writing_check = lambda instance: self.module.all_reduce(self.tensor, async_op=True)
            module = types.ModuleType("sglang.srt.mem_cache.hiradix_cache")
            module.HiRadixCache = Cache
            if semantic_first:
                Cache.writing_check = semantic(Cache.writing_check)
            probe.install(module)
            if not semantic_first:
                Cache.writing_check = semantic(Cache.writing_check)
            wrapped = Cache.writing_check
            probe.install(module)
            self.assertIs(Cache.writing_check, wrapped)
            self.assertIs(Cache().writing_check(), self.work)
        self.assertEqual(len(self.records()), 2)
        self.assertTrue(all(row["role"] == "write_completion_check" for row in self.records()))
        self.work.wait.assert_not_called()
