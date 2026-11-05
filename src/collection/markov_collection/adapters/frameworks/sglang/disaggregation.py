"""Capture native PD ownership transitions alongside the common model observation."""

from __future__ import annotations

from contextlib import contextmanager, nullcontext
from contextvars import ContextVar, copy_context
from functools import partial
from typing import Any

from markov_collection.artifacts import write_json


class DisaggregationObservation:
    def __init__(self, serving: Any, role: str, description: dict[str, Any]) -> None:
        from torch.utils.weak import WeakIdKeyDictionary

        from markov_collection.adapters.runtimes.ascend.transfer import MemfabricObservation

        self.serving = serving
        self.role = role
        self.recording = serving.recording
        self.operators = serving.operators
        self.values = self.operators.values
        self.inputs = {
            room: {"request": identity, "room": room, "parameters": {"B": 1, "M": request["prompt_tokens"]}}
            for room, identity, request in zip(
                description["source_bootstrap_rooms"], description["source_requests"],
                description["content"]["requests"], strict=True,
            )
        }
        self.chunks: Any = WeakIdKeyDictionary()
        self.current_transfer: ContextVar[tuple[str, int] | None] = ContextVar("markov_transfer", default=None)
        self.transport = MemfabricObservation(self.recording)

    def advertise(
        self, original: Any, receiver: Any, kv_indices: Any, aux_index=None, state_indices=None, decode_prefix_len=None,
    ) -> Any:
        metadata = self.inputs.get(receiver.bootstrap_room) if self.serving.selected else None
        if metadata is None:
            return original(receiver, kv_indices, aux_index, state_indices, decode_prefix_len)
        with self.recording.scope(
            "KvAdvertise", **metadata, lane=self.serving.batch_lane, entrypoint="kv_advertise", entrypoint_phase=None,
            ports={"inputs": {}, "outputs": {}},
        ) as scope:
            scope.metadata["parameters"] = {**metadata["parameters"], "metadata_slot": int(aux_index)}
            return original(receiver, kv_indices, aux_index, state_indices, decode_prefix_len)

    def status(self, original: Any, manager: Any, bootstrap_room: int, status: int) -> Any:
        from sglang.srt.disaggregation.base.conn import KVPoll

        metadata = self.inputs.get(bootstrap_room) if self.serving.selected else None
        connecting = self.role == "prefill" and status == KVPoll.WaitingForInput
        if metadata is None or not (connecting or status == KVPoll.Success):
            return original(manager, bootstrap_room, status)
        previous = manager.request_status.get(bootstrap_room)
        if previous is not None and previous >= status:
            return original(manager, bootstrap_room, status)

        # Observe real endpoint transitions, not repeated writes or the first successful scheduler poll.
        with self.recording.scope(
            "KvReady" if connecting else "KvComplete", **metadata,
            entrypoint="kv_ready" if connecting else "kv_complete", entrypoint_phase=None,
            ports={"inputs": {}, "outputs": {}},
        ):
            return original(manager, bootstrap_room, status)

    def registration(self, original: Any, queue: Any) -> Any:
        manager = original(queue)

        # Bind after native transport creation; installing hooks must not import its SDK early.
        if self.role == "prefill":
            manager.send_kvcache = partial(self.transfer, manager.send_kvcache, grouping="buffers")
            manager.send_kvcache_slice = partial(self.transfer, manager.send_kvcache_slice, grouping="buffer_tokens")
        if self.serving.selected:
            self.transport.install()

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

        # Process addresses identify observed copies; they never become template or target identities.
        write_json(
            self.serving.directory / f"transport-rank-{self.serving.parallel['rank']}.json",
            {
                "session": manager.engine.get_session_id(),
                "rank": self.serving.parallel["rank"],
                "kv_layout": self.serving.parallel["kv_layout"],
                "kv_addresses": list(manager.kv_args.kv_data_ptrs),
                "kv_bytes": list(manager.kv_args.kv_data_lens),
                "metadata_addresses": list(manager.kv_args.aux_data_ptrs),
                "metadata_bytes": list(manager.kv_args.aux_data_lens),
            },
        )
        return manager

    def poll_readiness(self, original: Any, pollers: Any) -> Any:
        if (
            not self.serving.selected or not pollers
            or any(poller.bootstrap_room not in self.inputs for poller in pollers)
        ):
            return original(pollers)

        # This native helper only samples local states; TP/PP consensus and request selection stay outside it.
        with self.recording.scope(
            "KvPoll", requests=[self.inputs[poller.bootstrap_room]["request"] for poller in pollers],
            lane=self.serving.batch_lane, entrypoint="kv_poll", entrypoint_phase=None,
            ports={"inputs": {}, "outputs": {}}, parameters={"B": len(pollers)},
        ) as scope:
            result = original(pollers)
            scope.metadata["statuses"] = result.copy()
            return result

    def request_metadata(self, request: Any, pool: Any) -> dict[str, Any]:
        prompt = len(request.origin_input_ids)
        lane = self.serving.batch_lane
        parent = self.recording.current_scope.get()
        while parent is not None:
            region = self.recording.regions[parent]
            if "batch" in region["metadata"]:
                lane = self.recording.batches[region["metadata"]["batch"]]["lane"]
                break
            parent = region["parent"]
        return {
            "request": request.rid,
            "room": request.bootstrap_room,
            "lane": lane,
            "parameters": {
                "B": 1,
                "M": prompt,
                "R": pool.size,
                "slot_capacity": pool.req_to_token.shape[0] - 1,
                "max_sequence_tokens": prompt,
            },
        }

    def reserve(self, original: Any, queue: Any, request: Any, *args: Any, **kwargs: Any) -> Any:
        if not self.serving.selected or request.rid not in self.serving.requests:
            return original(queue, request, *args, **kwargs)

        self.serving.begin_batch(queue.scheduler)
        with (
            self.recording.scope("KvReserve", **self.request_metadata(request, queue.req_to_token_pool)) as scope,
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
                "KvReceived", **self.request_metadata(request, queue.scheduler.req_to_token_pool)
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
            scope.metadata.update(dynamic_calls=True, parameters={"accepted_members": len(result.reqs)})
            scope.metadata["ports"] = {
                "inputs": {},
                "outputs": self.values.port_values(
                    {
                        **BatchTensors.read(result),
                        "token_relay": scheduler.future_map.output_tokens_buf,
                        "free_pages": scheduler.token_to_kv_pool_allocator.free_pages,
                        "request_map": scheduler.req_to_token_pool.req_to_token,
                    }
                ),
            }
            return result

    def prepare_prebuilt(self, original: Any, batch: Any) -> Any:
        from markov_collection.adapters.frameworks.sglang.prebuilt import PrebuiltCall

        return PrebuiltCall(self.values, original, batch, prepare=True)(batch)

    def process_prebuilt(self, original: Any, batch: Any, server_args: Any, future_map: Any) -> Any:
        from markov_collection.adapters.frameworks.sglang.prebuilt import PrebuiltCall

        return PrebuiltCall(self.values, original, batch, prepare=False, relay=future_map)(
            batch, server_args, future_map
        )

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

    def send(self, original: Any, scheduler: Any, request: Any, last_chunk=False, end_idx=None) -> Any:
        if not self.serving.selected or request.rid not in self.serving.requests:
            return original(scheduler, request, last_chunk=last_chunk, end_idx=end_idx)

        from markov_collection.adapters.runtimes.pytorch.indexing import TensorViewObservation

        metadata = self.request_metadata(request, scheduler.req_to_token_pool)
        begin = request.start_send_idx
        end = min(request.extend_range.end, len(request.origin_input_ids)) if end_idx is None else end_idx
        if not last_chunk:
            end -= end % scheduler.token_to_kv_pool_allocator.page_size
        if end < begin:
            return original(scheduler, request, last_chunk=last_chunk, end_idx=end_idx)
        metadata["parameters"].update(
            M=end - begin,
            send_begin=begin,
            max_sequence_tokens=end,
            last_chunk=int(last_chunk),
            metadata_slot=request.metadata_buffer_index,
            handoff_id=request.bootstrap_room,
        )
        # PP can send the previous chunk outside both scheduling and result processing.
        observation = self.operators if self.recording.current_scope.get() is None else nullcontext()
        with observation, self.recording.scope("KvSend", **metadata) as scope:
            scope.metadata["send_region"] = scope.identity
            self.bind_metadata(scheduler.disagg_metadata_buffers, scope.metadata)

            with self.values.metadata():
                mapping = scheduler.req_to_token_pool.req_to_token
                slot = int(request.req_pool_idx)
                scope.metadata["parameters"]["request_slot"] = slot
                scope.metadata["bindings"] = {"request_map_stride": mapping.stride(0)}
                scope.metadata["ports"] = {"inputs": self.values.port_values({"request_map": mapping}), "outputs": {}}

                # Native sends complete pages until the final call carries the remaining tail.
                symbols = self.values.shapes
                view = TensorViewObservation(
                    self.values,
                    [(mapping, (symbols.symbol("M"),))],
                    start=symbols.symbol("request_slot") * symbols.symbol("request_map_stride")
                    + symbols.symbol("send_begin"),
                    index=(slot, slice(begin, end)),
                )

            with view:
                result = original(scheduler, request, last_chunk=last_chunk, end_idx=end_idx)
            if view.result is None:
                raise ValueError("KV send did not read its declared token slice")
            return result

    def bind_metadata(self, buffers: Any, metadata: dict[str, Any]) -> None:
        import torch

        with self.values.metadata():
            tensors = {name: value for name, value in vars(buffers).items() if isinstance(value, torch.Tensor)}

            # The native scheduler reserves two metadata slots per request slot on both instances.
            for tensor in tensors.values():
                self.values.bind_shape(tensor, (2 * self.values.shapes.symbol("R"), *tensor.shape[1:]))
            metadata["metadata_buffers"] = self.values.port_values(tensors)

    def enqueue(self, original: Any, manager: Any, room: int, indices: Any, *args: Any, **kwargs: Any) -> Any:
        region = self.recording.current_scope.get()
        baseline = self.recording.current_baseline.get()
        origin = None
        if region is not None:
            origin = ("region", region)
        elif baseline is not None:
            origin = ("baseline", baseline)
        token = self.current_transfer.set(origin)
        try:
            if region is not None:
                self.recording.regions[region]["metadata"]["source_pages"] = indices.tolist()

                # Publish intent before queueing; the worker may finish before this call returns.
                with self.recording.scope("KvSubmit"):
                    pass
            return original(manager, room, indices, *args, **kwargs)
        finally:
            self.current_transfer.reset(token)

    def enqueue_chunk(self, original: Any, queue: Any, chunk: Any) -> Any:
        origin = self.current_transfer.get()
        if origin is not None:
            self.chunks[chunk] = origin
        return original(queue, chunk)

    def next_chunk(self, original: Any, queue: Any) -> Any:

        # Observation follows the native queue item, even when its page array is replaced or sliced.
        self.current_transfer.set(None)
        chunk = original(queue)
        self.current_transfer.set(self.chunks.get(chunk))
        return chunk

    @contextmanager
    def transfer_scope(self, role: str) -> Any:
        origin = self.current_transfer.get()
        if origin is None:
            yield None
            return
        kind, identity = origin
        if kind == "baseline":
            with self.recording.baseline(identity):
                yield None
            return

        # Queue ownership carries request context, not the sender's ports, bindings or page observations.
        caller = self.recording.regions[identity]["metadata"]
        metadata = {field: caller[field] for field in ("request", "room", "lane", "send_region")}
        metadata["parameters"] = dict(caller["parameters"])
        with self.recording.scope(role, **metadata) as scope:
            yield scope

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
        with self.transfer_scope("KvTransfer") as scope:
            if scope is not None:
                scope.metadata.update(
                    source_pages=source.tolist(), destination_pages=destination.tolist(), destination_session=session,
                )
            result = original(session, source, destination_buffers, destination, *args)
            if scope is not None:
                scope.metadata["status"] = result
            return result

    def metadata_transfer(self, original: Any, manager: Any, request: Any, *args: Any, **kwargs: Any) -> Any:
        with self.transfer_scope("KvMetadataTransfer") as scope:
            if scope is not None:
                scope.metadata.update(
                    destination_session=request.mooncake_session_id, destination_metadata_slot=request.dst_aux_index,
                )
            result = original(manager, request, *args, **kwargs)
            if scope is not None:
                scope.metadata["status"] = result
            return result

    def release(self, original: Any, request: Any, cache: Any, *args: Any, **kwargs: Any) -> Any:
        if not self.serving.selected or request.rid not in self.serving.requests:
            return original(request, cache, *args, **kwargs)

        metadata = self.request_metadata(request, cache.req_to_token_pool)
        metadata["parameters"]["resident_tokens"] = request._cache_commit_len()
        observation = self.operators if self.recording.current_scope.get() is None else nullcontext()
        with (
            self.recording.scope("KvReleased", **metadata),
            observation,
        ):

            # The native helper owns cache release, tail trimming and slot release as one operation.
            return original(request, cache, *args, **kwargs)

    def submit(self, original: Any, executor: Any, function: Any, *args: Any, **kwargs: Any) -> Any:
        scope = self.recording.current_scope.get()
        source = scope is not None and self.recording.regions[scope]["role"] in {"KvTransfer", "KvMetadataTransfer"}
        if not source and self.recording.current_baseline.get() is None:
            return original(executor, function, *args, **kwargs)

        # Carry observation ownership into native copy workers without changing their lifecycle.
        return original(executor, copy_context().run, function, *args, **kwargs)

    def install(self) -> None:
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        targets = {
            "sglang.srt.disaggregation.common.conn.CommonKVManager.update_status": self.status,
            "sglang.srt.disaggregation.mooncake.conn.MooncakeKVReceiver.send_metadata": self.advertise,
            "sglang.srt.mem_cache.common.release_kv_cache": self.release,
            "sglang.srt.disaggregation.utils._poll_with_failure_injection": self.poll_readiness,
            "concurrent.futures.ThreadPoolExecutor.submit": self.submit,
            "sglang.srt.disaggregation.common.utils.FastQueue.put": self.enqueue_chunk,
            "sglang.srt.disaggregation.common.utils.FastQueue.get": self.next_chunk,
            "sglang.srt.disaggregation.prefill.PrefillBootstrapQueue._init_kv_manager": self.registration,
            "sglang.srt.disaggregation.decode.DecodePreallocQueue._init_kv_manager": self.registration,
            "sglang.srt.disaggregation.decode.DecodePreallocQueue._pre_alloc": self.reserve,
            "sglang.srt.disaggregation.decode.DecodeTransferQueue._commit_transfer_to_req": self.receive,
            "sglang.srt.managers.scheduler.Scheduler.get_new_prebuilt_batch": self.prebuilt,
            "sglang.srt.managers.schedule_batch.ScheduleBatch.prepare_for_prebuilt": self.prepare_prebuilt,
            "sglang.srt.managers.schedule_batch.ScheduleBatch.process_prebuilt": self.process_prebuilt,
            (
                "sglang.srt.managers.scheduler_components.batch_result_processor."
                "SchedulerBatchResultProcessor.process_batch_result_prebuilt"
            ): self.prebuilt_result,
            "sglang.srt.managers.scheduler.Scheduler.send_kv_chunk": self.send,
            "sglang.srt.disaggregation.mooncake.conn.MooncakeKVManager.add_transfer_request": self.enqueue,
            "sglang.srt.disaggregation.mooncake.conn.MooncakeKVManager.send_aux": self.metadata_transfer,
            (
                "sglang.srt.distributed.device_communicators.mooncake_transfer_engine."
                "MooncakeTransferEngine.batch_transfer_sync"
            ): self.transport.copy,
        }
        for target, observer in targets.items():
            HookRegistry.register(target, observer, HookType.AROUND)
