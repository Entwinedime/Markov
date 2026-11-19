"""Observe native PD queue ownership and synchronous transport service boundaries."""

import inspect
from contextvars import ContextVar, copy_context
from functools import partial
from time import CLOCK_MONOTONIC_RAW, clock_gettime_ns


class ObservedExecutor:
    def __init__(self, executor) -> None:
        self.executor = executor

    def submit(self, operation, *args, **kwargs):

        # Native workers remain native; only the observed caller identity follows each submitted job.
        return self.executor.submit(copy_context().run, operation, *args, **kwargs)


class ReservationSocket:
    def __init__(self, owner, socket, receiver, peer) -> None:
        self.owner, self.socket, self.receiver, self.peer = owner, socket, receiver, peer

    def send_multipart(self, *args, **kwargs):
        return self.owner.send_reservation(self.socket.send_multipart, self.receiver, self.peer, args, kwargs)


class NativeKvScopes:
    def __init__(self, calls, observation=None) -> None:
        self.calls = calls
        self.observation = observation
        self.chunk = ContextVar("native_kv_chunk", default=None)
        self.service = ContextVar("native_kv_service", default=None)
        self.request = ContextVar("native_kv_request", default=None)
        self.reservation = ContextVar("native_kv_reservation", default=None)
        self.manager = None
        self.reservations = {}
        self.receipts = []
        self.states = []

    def update_status(self, original, manager, bootstrap_room, status):
        owner = self.observation
        request = owner.description["protocol_requests"].get(str(bootstrap_room))
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
            socket = ReservationSocket(self, socket, receiver, peer)
        return socket, lock

    def receive_reservation(self, original, cls, message):
        result = original(cls, message)
        owner = self.observation
        request = owner.description["protocol_requests"].get(str(result.room))
        if owner.selected and request is not None:

            # The receiver inserts the parsed contribution before reading the next reservation.
            self.receipts.append({"request": request, "session": result.mooncake_session_id,
                                  "received_raw_ns": clock_gettime_ns(CLOCK_MONOTONIC_RAW)})
        return result

    def send_reservation(self, original, receiver, peer, args, kwargs):
        work = self.calls.request_work(receiver._markov_request)
        work["context"]["reservation_peer"] = receiver.bootstrap_infos.index(peer)
        owner = self.observation
        with self.calls.separate("kv_reservation", work):
            if owner is None:
                with self.calls.native_call("kv_reservation", original, work=work):
                    return original(*args, **kwargs)

            with owner.call("kv_reservation", original, parameters={}, work=work) as call:
                scope = call.record
                scope["bindings"]["action"] = True

                # Remote visibility can begin as soon as the complete payload is handed to the socket.
                scope["ports"].append({"name": "reservation.ready", "direction": "output", "use": "signal",
                                       "boundary": {"scope": scope["id"], "interval": "call", "side": "begin"}})
                publication = {"request": work["context"]["request"]["id"], "target": [peer["rank_ip"], peer["rank_port"]]}
                scope["reservation_publication"] = publication
                with owner.capture.runtime.service_scope(publication):
                    return original(*args, **kwargs)

    def poll(self, original, transport):
        if not self.calls.enabled:
            return original(transport)

        work = self.calls.request_work(transport._markov_request)
        with self.calls.read("transport", original, work) as query:
            result = original(transport)
            query["result"] = result
        return result

    def reserve(self, original, receiver, kv_indices, aux_index=None, state_indices=None, decode_prefix_len=None):
        owner = self.observation
        request = receiver._markov_request
        if owner is not None:
            if owner.resource_history is not None:
                owner.resource_history.state_binding("metadata", request, aux_index)
            if request.rid in self.calls.aliases:
                self.reservations[str(receiver.bootstrap_room)] = {"request": self.calls.request_id(request.rid), "index": aux_index}
        if not self.calls.enabled:
            return original(receiver, kv_indices, aux_index, state_indices, decode_prefix_len)

        work = {"context": {"request": self.calls.progress((request,))[0]}}
        with self.calls.call("kv_reserve", original, parameters={"transfer_pages": len(kv_indices)}, work=work):
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
        owner = self.observation
        if owner is not None and owner.resource_history is not None:
            owner.resource_history.bind_field("metadata", allocator)
        self.calls.resources.bind_field("metadata", allocator)

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
        if not self.calls.enabled or not isinstance(item, TransferKVChunk):
            return original(queue, item)
        request, page_size = self.request.get()
        begin = request.start_send_idx // page_size * page_size
        end = min(begin + len(item.prefill_kv_indices) * page_size, len(request.origin_input_ids))
        work = self.calls.request_work(request, begin, end)
        parameters = {"transfer_pages": len(item.prefill_kv_indices), "transfer_tokens": end - begin, "last_chunk": int(item.is_last_chunk)}

        # FastQueue uses a reentrant condition: publish the completed marker before a consumer can take the task.
        with queue._cond:
            with self.calls.call(
                "kv_enqueue", original, parameters=parameters, work=work,
                guards=(
                    ["eq", "last_chunk", parameters["last_chunk"]],
                    ["lt", 0, "transfer_pages"] if len(item.prefill_kv_indices) else ["eq", "transfer_pages", 0],
                ),
            ):
                if owner is not None:
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
            finally:
                self.service.reset(previous)

        # Publish observed geometry after the measured service returns so processing the evidence adds no cost.
        self.payload(call.record)
        return result

    @staticmethod
    def payload(scope):
        transfer = scope["native_transfer"]
        transferred_bytes = sum(length for _, _, length in transfer["blocks"])
        scope["service"]["payload"] = {"bytes": transferred_bytes, "direction": "peer_device"}
        scope["parameters"] = {"transfer_bytes": transferred_bytes}

        # State is indexed by logical tokens; the physical transport sends full pages or fixed metadata rows.
        pages = len(transfer.get("source_pages", ()))
        if pages:
            page_bytes, tail = divmod(transferred_bytes, pages)
            if tail or not page_bytes:
                raise ValueError("native KV transport has no complete per-page byte extent")
            scope["arguments"] = {"transfer_bytes": ["mul", "transfer_pages", page_bytes]}
            scope["constants"] = {}
        else:
            scope["arguments"] = {}
            scope["constants"] = {"transfer_bytes": transferred_bytes}

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

        for target, hook in (
            ("sglang.srt.disaggregation.utils.ReqToMetadataIdxAllocator.__init__", self.metadata_allocator),
            ("sglang.srt.disaggregation.common.conn.CommonKVReceiver._connect_to_bootstrap_server", self.connect_reservation),
            ("sglang.srt.disaggregation.mooncake.conn.MooncakeKVSender.poll", self.poll),
            ("sglang.srt.disaggregation.mooncake.conn.MooncakeKVReceiver.poll", self.poll),
            ("sglang.srt.managers.scheduler.Scheduler.send_kv_chunk", self.send_chunk),
            ("sglang.srt.disaggregation.mooncake.conn.MooncakeKVReceiver.send_metadata", self.reserve),
            ("sglang.srt.disaggregation.common.utils.FastQueue.put", self.enqueue),
        ):
            HookRegistry.register(target, hook, HookType.AROUND)
        if self.observation is None:
            return

        from sglang.srt.disaggregation.ascend.conn import AscendKVManager
        from sglang.srt.disaggregation.mooncake.conn import MooncakeKVManager

        for target, hook in (
            ("sglang.srt.disaggregation.common.conn.CommonKVManager.update_status", self.update_status),
            ("sglang.srt.disaggregation.mooncake.conn.MooncakeKVManager.__init__", self.manager_init),
            ("sglang.srt.disaggregation.mooncake.conn.TransferInfo.from_zmq", self.receive_reservation),
            ("sglang.srt.disaggregation.utils.MetadataBuffers.set_buf", self.metadata),
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
