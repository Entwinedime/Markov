"""Capture native PD ownership transitions alongside the common model observation."""

from __future__ import annotations

from contextvars import copy_context
from functools import partial
from typing import Any


class DisaggregationObservation:
    def __init__(self, serving: Any, role: str) -> None:
        from markov_collection.adapters.runtimes.ascend.transfer import MemfabricObservation

        self.serving = serving
        self.role = role
        self.recording = serving.recording
        self.operators = serving.operators
        self.values = self.operators.values
        self.rooms: dict[int, dict[str, Any]] = {}
        self.pages: dict[tuple[int, ...], dict[str, Any]] = {}
        self.transport = MemfabricObservation(self.recording)

    def registration(self, original: Any, queue: Any) -> Any:
        manager = original(queue)

        # Bind after native transport creation; installing hooks must not import its SDK early.
        if self.role == "prefill":
            manager.send_kvcache = partial(self.transfer, manager.send_kvcache, grouping="buffers")
            manager.send_kvcache_slice = partial(self.transfer, manager.send_kvcache_slice, grouping="buffer_tokens")

        pool = queue.token_to_kv_pool
        heads = queue.scheduler.model_config.get_total_num_kv_heads()
        first = manager.attn_tp_rank // max(1, manager.attn_tp_size // heads) * pool.head_num
        tensors = {}
        for layer in range(pool.start_layer, pool.start_layer + pool.layer_num):
            for kind, tensor in (("key", pool._get_key_buffer(layer)), ("value", pool._get_value_buffer(layer))):
                tensors[tensor.data_ptr()] = kind, layer, tensor

        buffers = []
        for address in manager.kv_args.kv_data_ptrs:
            kind, layer, tensor = tensors[address]
            buffers.append(
                {
                    "kind": kind,
                    "layer": layer,
                    "head_begin": first,
                    "head_end": first + pool.head_num,
                    "head_bytes": tensor.stride(-2) * tensor.element_size(),
                }
            )
        self.serving.parallel["kv_layout"] = {
            "page_size": manager.kv_args.page_size,
            "buffers": buffers,
            "metadata_item_bytes": list(manager.kv_args.aux_item_lens),
        }
        return manager

    def poll_readiness(self, original: Any, *args: Any, **kwargs: Any) -> Any:
        if self.recording.current_scope.get() is None:
            return original(*args, **kwargs)

        # Native status consensus selects requests; the target controller owns that decision.
        with self.recording.scope("ReadinessPoll", control_only=True, path=original.__qualname__):
            return original(*args, **kwargs)

    @staticmethod
    def request_metadata(request: Any, capacity: int) -> dict[str, Any]:
        prompt = len(request.origin_input_ids)
        return {
            "request": request.rid,
            "room": request.bootstrap_room,
            "parameters": {"B": 1, "M": prompt, "R": capacity, "max_sequence_tokens": prompt},
        }

    def reserve(self, original: Any, queue: Any, request: Any, *args: Any, **kwargs: Any) -> Any:
        if not self.serving.selected or request.rid not in self.serving.requests:
            return original(queue, request, *args, **kwargs)

        self.serving.begin_batch(queue.scheduler)
        with (
            self.recording.scope("KvReserve", **self.request_metadata(request, queue.req_to_token_pool.size)) as scope,
            self.operators,
        ):
            scope.metadata["ports"] = {
                "inputs": self.values.port_values(
                    {
                        "request_map": queue.req_to_token_pool.req_to_token,
                        "free_pages": queue.token_to_kv_pool_allocator.free_pages,
                    }
                ),
                "outputs": {},
            }
            result = original(queue, request, *args, **kwargs)
            scope.metadata["slot"] = int(request.req_pool_idx)
            scope.metadata["ports"]["outputs"] = self.values.port_values(
                {
                    "kv_locations": result,
                    "request_map": queue.req_to_token_pool.req_to_token,
                    "free_pages": queue.token_to_kv_pool_allocator.free_pages,
                }
            )
            return result

    def receive(self, original: Any, queue: Any, transfer: Any) -> Any:
        request = transfer.req
        if not self.serving.selected or request.rid not in self.serving.requests:
            return original(queue, transfer)

        with (
            self.recording.scope(
                "KvReceived", **self.request_metadata(request, queue.scheduler.req_to_token_pool.size)
            ) as scope,
            self.operators,
        ):
            self.bind_metadata(queue.metadata_buffers, scope.metadata)
            result = original(queue, transfer)

            # The native metadata gate has completed before this first token is committed.
            scope.metadata["generated_tokens"] = len(request.output_ids)
            scope.metadata["slot"] = int(request.req_pool_idx)
            return result

    def prebuilt(self, original: Any, scheduler: Any) -> Any:
        from markov_collection.adapters.frameworks.sglang.batches import BatchTensors

        if self.serving.observation(scheduler.waiting_queue) != "source":
            return original(scheduler)

        # The common scheduling boundary already owns dispatcher observation.
        with self.recording.scope("BatchAccepted", entrypoint="batch_accept") as scope:
            result = original(scheduler)
            if result is None:
                scope.metadata.pop("entrypoint")
                return result

            scope.metadata["batch"] = self.serving.record_batch(scheduler, result, "source")
            scope.metadata["ports"] = {
                "inputs": {},
                "outputs": self.values.port_values(
                    {
                        **BatchTensors.read(result),
                        "token_relay": scheduler.future_map.output_tokens_buf,
                        "free_pages": scheduler.token_to_kv_pool_allocator.free_pages,
                    }
                ),
            }
            return result

    def prebuilt_result(self, original: Any, processor: Any, batch: Any) -> Any:
        identity = self.serving.batch_ids.get(batch)
        if identity is None:
            return original(processor, batch)

        with self.recording.scope("PrebuiltResult", batch=identity) as scope:
            scope.metadata["ports"] = {
                "inputs": self.values.port_values({"free_pages": processor.token_to_kv_pool_allocator.free_pages}),
                "outputs": {},
            }
            result = original(processor, batch)
            scope.metadata["ports"]["outputs"] = self.values.port_values(
                {"free_pages": processor.token_to_kv_pool_allocator.free_pages}
            )
            scope.metadata["effects"] = [
                {"id": request.rid, "generated_tokens": len(request.output_ids), "finished": request.finished()}
                for request in batch.reqs
            ]
            return result

    def send(self, original: Any, scheduler: Any, request: Any, *args: Any, **kwargs: Any) -> Any:
        if not self.serving.selected or request.rid not in self.serving.requests:
            return original(scheduler, request, *args, **kwargs)

        from markov_collection.adapters.runtimes.pytorch.indexing import TensorViewObservation

        metadata = self.request_metadata(request, scheduler.req_to_token_pool.size)
        self.rooms[request.bootstrap_room] = metadata
        with self.recording.scope("KvSend", **metadata) as scope:
            self.bind_metadata(scheduler.disagg_metadata_buffers, scope.metadata)

            with self.values.metadata():
                mapping = scheduler.req_to_token_pool.req_to_token
                slot = int(request.req_pool_idx)
                scope.metadata["parameters"]["request_slot"] = slot
                scope.metadata["bindings"] = {"request_map_stride": mapping.stride(0)}
                scope.metadata["ports"] = {
                    "inputs": self.values.port_values({"request_map": mapping}),
                    "outputs": {}
                }

                # Declare the complete-prefill slice before CPU materialization and its downstream views.
                symbols = self.values.shapes
                view = TensorViewObservation(
                    self.values,
                    [(mapping, (symbols.symbol("M"),))],
                    start=symbols.symbol("request_slot") * symbols.symbol("request_map_stride"),
                    index=(slot, slice(0, len(request.origin_input_ids))),
                )

            with view:
                result = original(scheduler, request, *args, **kwargs)
            if view.result is None:
                raise ValueError("KV send did not read its declared complete-prefill slice")
            return result

    def bind_metadata(self, buffers: Any, metadata: dict[str, Any]) -> None:
        import torch

        with self.values.metadata():
            tensors = {name: value for name, value in vars(buffers).items() if isinstance(value, torch.Tensor)}

            # The native scheduler reserves two metadata slots per request slot on both instances.
            for tensor in tensors.values():
                self.values.bind_shape(tensor, (2 * self.values.shapes.symbol("R"), *tensor.shape[1:]))
            metadata["metadata_buffers"] = self.values.port_values(tensors)

    def enqueue(self, original: Any, sender: Any, indices: Any, *args: Any, **kwargs: Any) -> Any:
        metadata = self.rooms.get(sender.bootstrap_room)
        if metadata is not None:
            self.pages[tuple(map(int, indices))] = metadata
            scope = self.recording.regions[self.recording.current_scope.get()]["metadata"]
            scope["source_pages"] = indices.tolist()
        return original(sender, indices, *args, **kwargs)

    def transfer(
        self,
        original: Any,
        session: str,
        source: Any,
        destination_buffers: Any,
        destination: Any,
        *args: Any,
        grouping: str,
    ) -> Any:
        self.serving.parallel["kv_layout"]["copy_grouping"] = grouping
        metadata = self.pages.get(tuple(map(int, source)))
        if metadata is None:
            return original(session, source, destination_buffers, destination, *args)

        with self.recording.scope(
            "KvTransfer", **metadata, source_pages=source.tolist(), destination_pages=destination.tolist()
        ) as scope:
            result = original(session, source, destination_buffers, destination, *args)
            scope.metadata["status"] = result
            return result

    def metadata_transfer(self, original: Any, manager: Any, request: Any, *args: Any, **kwargs: Any) -> Any:
        metadata = self.rooms.get(request.room)
        if metadata is None:
            return original(manager, request, *args, **kwargs)

        with self.recording.scope("KvMetadataTransfer", **metadata):
            return original(manager, request, *args, **kwargs)

    def release(self, original: Any, cache: Any, request: Any, *args: Any, **kwargs: Any) -> Any:
        if not self.serving.selected:
            return original(cache, request, *args, **kwargs)
        with (
            self.recording.scope("KvReleased", **self.request_metadata(request, cache.req_to_token_pool.size)),
            self.operators,
        ):
            return self.serving.request_cache(original, cache, request, *args, **kwargs)

    def submit(self, original: Any, executor: Any, function: Any, *args: Any, **kwargs: Any) -> Any:
        scope = self.recording.current_scope.get()
        if scope is None or self.recording.regions[scope]["role"] != "KvTransfer":
            return original(executor, function, *args, **kwargs)

        # Carry observation ownership into native copy workers without changing their lifecycle.
        return original(executor, copy_context().run, function, *args, **kwargs)

    def install(self) -> None:
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        targets = {
            "sglang.srt.disaggregation.prefill.poll_and_all_reduce_attn_cp_tp_group": self.poll_readiness,
            "concurrent.futures.ThreadPoolExecutor.submit": self.submit,
            "sglang.srt.disaggregation.prefill.PrefillBootstrapQueue._init_kv_manager": self.registration,
            "sglang.srt.disaggregation.decode.DecodePreallocQueue._init_kv_manager": self.registration,
            "sglang.srt.disaggregation.decode.DecodePreallocQueue._pre_alloc": self.reserve,
            "sglang.srt.disaggregation.decode.DecodeTransferQueue._commit_transfer_to_req": self.receive,
            "sglang.srt.managers.scheduler.Scheduler.get_new_prebuilt_batch": self.prebuilt,
            (
                "sglang.srt.managers.scheduler_components.batch_result_processor."
                "SchedulerBatchResultProcessor.process_batch_result_prebuilt"
            ): self.prebuilt_result,
            "sglang.srt.managers.scheduler.Scheduler.send_kv_chunk": self.send,
            "sglang.srt.disaggregation.mooncake.conn.MooncakeKVSender.send": self.enqueue,
            "sglang.srt.disaggregation.mooncake.conn.MooncakeKVManager.send_aux": self.metadata_transfer,
            (
                "sglang.srt.distributed.device_communicators.mooncake_transfer_engine."
                "MooncakeTransferEngine.batch_transfer_sync"
            ): self.transport.copy,
        }
        for target, observer in targets.items():
            HookRegistry.register(target, observer, HookType.AROUND)
