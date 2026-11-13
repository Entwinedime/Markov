"""Observe native PD queue ownership and synchronous transport service boundaries."""

import inspect
from contextvars import ContextVar, copy_context
from functools import partial


class ObservedExecutor:
    def __init__(self, executor) -> None:
        self.executor = executor

    def submit(self, operation, *args, **kwargs):

        # Native workers remain native; only the observed caller identity follows each submitted job.
        return self.executor.submit(copy_context().run, operation, *args, **kwargs)


class ReservationSocket:
    def __init__(self, observation, socket, receiver, peer) -> None:
        self.observation, self.socket, self.receiver, self.peer = observation, socket, receiver, peer

    def send_multipart(self, *args, **kwargs):
        owner, receiver = self.observation, self.receiver
        work = owner.calls.request_work(receiver._markov_request)
        work["context"]["reservation_peer"] = receiver.bootstrap_infos.index(self.peer)
        with owner.calls.separate("kv_reservation", work), owner.call(
            "kv_reservation", self.socket.send_multipart, parameters={}, work=work,
        ) as call:
            scope = call.record
            scope["bindings"]["action"] = True

            # The payload exists on entry; remote visibility can occur before this local send returns.
            scope["ports"].append({"name": "reservation.ready", "direction": "output", "use": "signal",
                                   "boundary": {"scope": scope["id"], "interval": "call", "side": "begin"}})
            scope["reservation_publication"] = {
                "request": work["context"]["request"]["id"],
                "target": [self.peer["rank_ip"], self.peer["rank_port"]],
            }
            with owner.capture.runtime.service_scope(scope["reservation_publication"]):
                return self.socket.send_multipart(*args, **kwargs)


class NativeKvScopes:
    def __init__(self, observation) -> None:
        self.observation = observation
        self.chunk = ContextVar("native_kv_chunk", default=None)
        self.service = ContextVar("native_kv_service", default=None)
        self.request = ContextVar("native_kv_request", default=None)
        self.reservation = ContextVar("native_kv_reservation", default=None)
        self.manager = None
        self.reservations = {}
        self.requests = observation.description["protocol_requests"]
        self.states = []

    def update_status(self, original, manager, bootstrap_room, status):
        owner = self.observation
        request = self.requests.get(str(bootstrap_room))
        if not owner.selected or request is None:
            return original(manager, bootstrap_room, status)

        # Source rooms are known before profiler activation; warmup rooms are absent from this submission map.
        record = {"request": request}
        with owner.capture.runtime.service_scope(record):
            record["before"] = manager.request_status.get(bootstrap_room)
            result = original(manager, bootstrap_room, status)
            record["after"] = manager.request_status.get(bootstrap_room)
        self.states.append(record)
        return result

    def connect_reservation(self, original, cls, peer):
        socket, lock = original(cls, peer)
        receiver = self.reservation.get()
        if receiver is not None:
            socket = ReservationSocket(self.observation, socket, receiver, peer)
        return socket, lock

    def poll(self, original, transport):
        owner = self.observation
        if not owner.active or not owner.selected:
            return original(transport)
        req = transport._markov_request

        work = owner.calls.request_work(req)
        with owner.calls.read("transport", original, work) as query:
            result = original(transport)
            query["result"] = result
        return result

    def reserve(self, original, receiver, kv_indices, aux_index=None, state_indices=None, decode_prefix_len=None):
        owner = self.observation
        room = receiver.bootstrap_room
        request = receiver._markov_request
        if owner.resource_history is not None:
            owner.resource_history.state_binding("metadata", request, aux_index)
        if request.rid in owner.content.aliases:
            self.reservations[str(room)] = {"request": owner.content.aliases[request.rid], "index": aux_index}
        if not owner.active or not owner.selected:
            return original(receiver, kv_indices, aux_index, state_indices, decode_prefix_len)

        work = owner.calls.work(None)
        work["context"] = {"request": owner.calls.progress((request,))[0]}
        with owner.calls.call("kv_reserve", original, parameters={"transfer_pages": len(kv_indices)}, work=work):
            previous = self.reservation.set(receiver)
            try:
                return original(receiver, kv_indices, aux_index, state_indices, decode_prefix_len)
            finally:
                self.reservation.reset(previous)

    def metadata(self, original, buffers, req):
        aliases = self.observation.content.aliases
        if req.bootstrap_room is not None and req.rid in aliases:
            self.reservations[str(req.bootstrap_room)] = {"request": aliases[req.rid], "index": req.metadata_buffer_index}
        if self.observation.resource_history is not None:
            self.observation.resource_history.state_binding("metadata", req, req.metadata_buffer_index)
        return original(buffers, req)

    def metadata_allocator(self, original, allocator, size):
        original(allocator, size)
        if self.observation.resource_history is not None:
            self.observation.resource_history.bind_field("metadata", allocator)
        self.observation.calls.resources.bind_field("metadata", allocator)

    def manager_init(self, original, manager, *args, **kwargs):
        result = original(manager, *args, **kwargs)
        self.manager = manager
        return result

    def send_chunk(self, original, scheduler, req, *args, **kwargs):
        previous = self.request.set((req, scheduler.token_to_kv_pool_allocator.page_size))
        try:
            return original(scheduler, req, *args, **kwargs)
        finally:
            self.request.reset(previous)

    def enqueue(self, original, queue, item):
        from sglang.srt.disaggregation.common.utils import TransferKVChunk

        owner = self.observation
        if not owner.active or not owner.selected or not isinstance(item, TransferKVChunk):
            return original(queue, item)
        request, page_size = self.request.get()
        begin = request.start_send_idx // page_size * page_size
        end = min(begin + len(item.prefill_kv_indices) * page_size, len(request.origin_input_ids))
        work = owner.calls.request_work(request, begin, end)
        parameters = {"transfer_pages": len(item.prefill_kv_indices), "transfer_tokens": end - begin, "last_chunk": int(item.is_last_chunk)}

        # FastQueue uses a reentrant condition: publish the completed marker before a consumer can take the task.
        with queue._cond:
            with owner.calls.call(
                "kv_enqueue", original, parameters=parameters, work=work,
                guards=(
                    ["eq", "last_chunk", parameters["last_chunk"]],
                    ["lt", 0, "transfer_pages"] if len(item.prefill_kv_indices) else ["eq", "transfer_pages", 0],
                ),
            ):
                call = owner.capture.scopes[owner.capture.current.get()]
                item._markov_submission = call["id"]
                call["native_kv_chunk"] = {
                    "room": item.room, "source_pages": item.prefill_kv_indices.tolist(),
                    "metadata_index": item.prefill_aux_index, "request_slot": int(request.req_pool_idx) - 1,
                }
                return original(queue, item)

    def dequeue(self, original, queue):
        self.chunk.set(None)
        item = original(queue)
        if hasattr(item, "_markov_submission"):
            self.chunk.set(item)
        return item

    def transfer(self, signature, original, manager, *args, **kwargs):
        owner = self.observation
        chunk = self.chunk.get()
        if not owner.active or not owner.selected or chunk is None:
            return original(manager, *args, **kwargs)
        arguments = signature.bind(manager, *args, **kwargs)
        arguments.apply_defaults()
        values = arguments.arguments
        if "executor" in values:
            values["executor"] = ObservedExecutor(values["executor"])

        with owner.capture.call(
            "kv_transfer", original.__qualname__, f"{original.__module__}.{original.__qualname__}",
            parent=chunk._markov_submission, service={"kind": "transfer", "role": "kv_transport", "executions": []},
        ) as call:
            call.record["submitted_by"] = chunk._markov_submission
            call.record["native_transfer"] = {
                "kind": "metadata" if original.__name__ == "send_aux" else "kv", "blocks": [],
                "session": values["req"].mooncake_session_id if original.__name__ == "send_aux" else values["mooncake_session_id"],
            }
            if original.__name__ != "send_aux":
                call.record["native_transfer"].update(
                    source_pages=values["prefill_kv_indices"].tolist(), target_pages=values["dst_kv_indices"].tolist(),
                )
            else:
                call.record["native_transfer"].update(
                    room=values["req"].room, source_index=values["prefill_aux_index"], target_index=values["req"].dst_aux_index,
                )

                # The submitted request owns both metadata endpoints; interpretation only resolves their physical rows.
                member, = call.record["work"]["members"]
                call.record["work"].update({f"metadata/{mode}": [member["id"]] for mode in ("read", "write")})
            previous = self.service.set(call.record)
            try:
                result = original(*arguments.args, **arguments.kwargs)
                if result != 0:
                    raise RuntimeError(f"native KV service failed: {original.__qualname__}: {result}")
                return result
            finally:
                self.service.reset(previous)

    def transfer_data(self, original, engine, session_id, buffers, peer_buffer_addresses, lengths):
        call = self.service.get()
        if call is None:
            return original(engine, session_id, buffers, peer_buffer_addresses, lengths)
        execution = {}
        with self.observation.capture.lock:
            call["native_transfer"]["blocks"].extend([
                [int(source), int(target), int(length)]
                for source, target, length in zip(buffers, peer_buffer_addresses, lengths, strict=True)
            ])
            call["service"]["executions"].append(execution)
        with self.observation.capture.runtime.service_execution(call["id"], execution):
            return original(engine, session_id, buffers, peer_buffer_addresses, lengths)

    def registration(self):
        if self.manager is None:
            return None
        args = self.manager.kv_args
        return {
            "session": self.manager.engine.session_id,
            "control_endpoint": [self.manager.local_ip, self.manager.rank_port],
            "pointers": [int(value) for value in args.kv_data_ptrs],
            "lengths": [int(value) for value in args.kv_data_lens],
            "page_bytes": [int(value) for value in args.kv_item_lens],
            "metadata": {
                "pointers": [int(value) for value in args.aux_data_ptrs],
                "lengths": [int(value) for value in args.aux_data_lens],
                "row_bytes": [int(value) for value in args.aux_item_lens],
                "reservations": self.reservations,
            },
        }

    def install(self):
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType
        from sglang.srt.disaggregation.ascend.conn import AscendKVManager
        from sglang.srt.disaggregation.mooncake.conn import MooncakeKVManager

        for target, hook in (
            ("sglang.srt.disaggregation.utils.ReqToMetadataIdxAllocator.__init__", self.metadata_allocator),
            ("sglang.srt.disaggregation.common.conn.CommonKVManager.update_status", self.update_status),
            ("sglang.srt.disaggregation.common.conn.CommonKVReceiver._connect_to_bootstrap_server", self.connect_reservation),
            ("sglang.srt.disaggregation.mooncake.conn.MooncakeKVSender.poll", self.poll),
            ("sglang.srt.disaggregation.mooncake.conn.MooncakeKVReceiver.poll", self.poll),
            ("sglang.srt.managers.scheduler.Scheduler.send_kv_chunk", self.send_chunk),
            ("sglang.srt.disaggregation.mooncake.conn.MooncakeKVManager.__init__", self.manager_init),
            ("sglang.srt.disaggregation.mooncake.conn.MooncakeKVReceiver.send_metadata", self.reserve),
            ("sglang.srt.disaggregation.utils.MetadataBuffers.set_buf", self.metadata),
            ("sglang.srt.disaggregation.common.utils.FastQueue.put", self.enqueue),
            ("sglang.srt.disaggregation.common.utils.FastQueue.get", self.dequeue),
            ("sglang.srt.distributed.device_communicators.mooncake_transfer_engine.MooncakeTransferEngine.batch_transfer_sync", self.transfer_data),
        ):
            HookRegistry.register(target, hook, HookType.AROUND)
        for cls, method in (
            (AscendKVManager, "send_kvcache"),
            (MooncakeKVManager, "send_kvcache_slice"),
            (MooncakeKVManager, "send_aux"),
        ):
            HookRegistry.register(
                f"{cls.__module__}.{cls.__qualname__}.{method}",
                partial(self.transfer, inspect.signature(getattr(cls, method))), HookType.AROUND,
            )
