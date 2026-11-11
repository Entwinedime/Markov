"""Adapt native PD transport interfaces to graph completion and local control messages."""

from collections import defaultdict
from functools import partial

from markov_control.adapters.frameworks.sglang.kv_transfer import MetadataRows, PdTopology
from markov_control.adapters.frameworks.sglang.resources import StateSlots
from sglang.srt.disaggregation.base import BaseKVManager, BaseKVReceiver, KVArgs, KVPoll
from sglang.srt.disaggregation.common.conn import CommonKVManager, CommonKVSender
from sglang.srt.disaggregation.utils import KVClassType


class NativePdBackend:
    def __init__(self, scheduler, workers, local):
        self.scheduler = scheduler
        self.topology = PdTopology(workers, local)
        self.local = local
        self.senders = {}
        self.receivers = {}
        self.reservations = defaultdict(dict)
        self.published = {}
        self.sequence = 0
        self.failures = {}

    def exchange(self, deliveries=(), *, completion=None):
        for packet in self.scheduler.peer.exchange("messages", {"deliveries": list(deliveries), "completion": completion}):
            source, value = packet["source"], packet["value"]
            if source not in self.topology.peers and source != self.local:
                raise ValueError("PD message came from a worker outside its native transfer topology")
            kind = value["kind"]
            if kind == "reservation":
                self.reservations[value["room"]][source] = value
            elif kind == "reservation_visible":
                self.manager.update_status(value["room"], KVPoll.WaitingForInput)
                self.senders[value["room"]].completions[KVPoll.WaitingForInput] = packet["completion"]
            elif kind == "transfer":
                self.receivers[value["room"]].pending[source] = value
            elif kind == "sender_visible":
                self.manager.update_status(value["room"], KVPoll.Success)
                self.senders[value["room"]].completions[KVPoll.Success] = packet["completion"]
            elif kind == "receiver_visible":
                self.receivers[value["room"]].visible = True
                self.receivers[value["room"]].completions[KVPoll.Success] = packet["completion"]
            elif kind == "consumed":
                publication = self.published[value["publication"]]
                publication["pending"].remove(source)
                if not publication["pending"]:
                    del self.published[value["publication"]]
            elif kind == "failed":
                self.failures[value["room"]] = value["reason"]
            else:
                raise ValueError(f"unknown PD control message: {kind}")

    def live_signals(self):
        signals = {signal for value in self.published.values() for signal in value["signals"]}
        for sender in self.senders.values():
            signals.update(sender.signals)
        for receiver in self.receivers.values():
            signals.update(receiver.signals)
        return signals

    @staticmethod
    def reservation_signal(publisher, request, recipient):
        return f"{publisher}/pd-reservation/{request}/{recipient}"

    def waiting(self):
        transports = (*self.senders.values(), *self.receivers.values())

        # Local readiness may still await PP consensus; only native queues can turn it into runnable work.
        return bool(transports) and all(
            transport.poll_state in (KVPoll.Bootstrapping, KVPoll.WaitingForInput, KVPoll.Transferring, KVPoll.Success)
            for transport in transports
        )

    def failed(self, room):
        self.exchange()
        return room in self.failures

    def abort(self, room):
        reason = f"native PD request {room} was aborted"
        self.failures[room] = reason
        self.exchange([
            {"target": peer, "value": {"kind": "failed", "room": room, "reason": reason}}
            for peer in self.topology.peers
        ])

    @staticmethod
    def group_poll(original, pollers, *args, **kwargs):
        results = original(pollers, *args, **kwargs)

        # Keep the decision already consumed by native queues, including metadata gates and TP/CP agreement.
        for poller, result in zip(pollers, results, strict=True):
            poller.poll_state = result
        return results

    def install_resources(self):
        from sglang.srt.disaggregation import decode, prefill
        from sglang.srt.managers import scheduler

        # Replace the transport factory in this worker only; native queues and their control methods remain intact.
        decode.get_kv_class = prefill.get_kv_class = self.native_class
        scheduler.ReqToMetadataIdxAllocator = partial(StateSlots, peer=self.scheduler.peer, field="metadata")

    def install_calls(self):
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        # Serving hooks must join the graph wrappers before the registry patches their shared targets.
        HookRegistry.register("sglang.srt.disaggregation.utils.MetadataBuffers.set_buf", self.bind_metadata, HookType.AROUND)
        for name in ("poll_and_all_reduce", "poll_and_all_reduce_attn_cp_tp_group"):
            HookRegistry.register(f"sglang.srt.disaggregation.utils.{name}", self.group_poll, HookType.AROUND)

    def bind_metadata(self, original, buffers, req):

        # Native allocation precedes request admission; bind the row before its first graph write.
        self.scheduler.req_to_metadata_buffer_idx_allocator.bind(req, req.metadata_buffer_index)
        return original(buffers, req)

    def native_class(self, backend, kind):
        return {
            KVClassType.KVARGS: KVArgs,
            KVClassType.MANAGER: partial(NativeKvManager, backend=self),
            KVClassType.SENDER: NativeKvSender,
            KVClassType.RECEIVER: NativeKvReceiver,
        }[kind]


class NativeKvManager(BaseKVManager):
    def __init__(self, args, disaggregation_mode, server_args, is_mla_backend=False, *, backend):
        if is_mla_backend or args.state_types:
            raise ValueError("PD templates require the observed MHA pool without auxiliary state pools")
        self.backend = backend
        backend.manager = self
        self.kv_args = args
        self.server_args = server_args
        self.is_dummy_cp_rank = False
        self.enable_all_cp_ranks_for_transfer = False
        self.request_status = {}
        self.req_to_decode_prefix_len = {}
        self.prefill_info_table = {}
        self.kv_item_lens_sum = sum(args.kv_item_lens)
        self.state_item_lens_sum = 0
        self.rows = MetadataRows(backend.scheduler.disagg_metadata_buffers)

    check_status = CommonKVManager.check_status
    update_status = CommonKVManager.update_status

    def register_to_bootstrap(self):

        # The serving task already supplies both native worlds; device-free control does not open a bootstrap server.
        return None

    def try_ensure_parallel_info(self, address):
        self.prefill_info_table[address] = self.backend.topology.info
        return True

    def record_failure(self, room, reason):
        self.backend.failures[room] = reason


class NativeKvPolling:
    def poll(self):
        calls = self.backend.scheduler.calls
        if not calls.enabled:
            self.poll_state = self._poll()
            return self.poll_state

        with calls.read("transport", self.poll, calls.request_work(self._markov_request)) as query:
            self.poll_state = self._poll()

            # Record only the state actually read, not unrelated deliveries collected in the same inbox drain.
            completion = self.completions.get(self.poll_state)
            if completion is not None:
                self.backend.scheduler.peer.call("observed_completion", event=completion)
            query["result"] = self.poll_state
        return self.poll_state


class NativeKvSender(NativeKvPolling, CommonKVSender):
    def __init__(self, mgr, bootstrap_addr, bootstrap_room, dest_tp_ranks, pp_rank):
        super().__init__(mgr, bootstrap_addr, bootstrap_room, dest_tp_ranks, pp_rank)
        self.backend = mgr.backend
        self.signals = set()
        self.poll_state = None
        self.completions = {}
        self.backend.senders[bootstrap_room] = self

    def _poll(self):
        if self.backend.failed(self.bootstrap_room):
            return KVPoll.Failed
        if self.conclude_state is not None:
            return self.conclude_state
        state = self.kv_mgr.check_status(self.bootstrap_room)
        if state == KVPoll.Bootstrapping:
            return state
        reservations = self.backend.reservations[self.bootstrap_room]
        if self.aux_index is None:
            prefixes = {value["prefix"] for value in reservations.values()}
            if len(prefixes) != 1:
                raise ValueError("native PD destinations disagree on the reused prefix")
            self.kv_mgr.req_to_decode_prefix_len[self.bootstrap_room] = prefixes.pop()
            return KVPoll.WaitingForInput
        if state == KVPoll.Success:
            self.conclude_state = KVPoll.Success
            return self.conclude_state
        return KVPoll.WaitingForInput

    def send(self, kv_indices, state_indices=None):
        if state_indices:
            raise ValueError("PD transfer template has no auxiliary state-pool contract")
        kv_indices, index_slice, last, _ = self._prepare_send_indices(kv_indices, state_indices)
        self._record_transfer_indices(kv_indices, state_indices)
        backend, scheduler = self.backend, self.backend.scheduler
        req = scheduler.calls.current.get().inputs
        page_size = self.kv_mgr.kv_args.page_size
        begin = req.start_send_idx // page_size * page_size
        end = min(begin + len(kv_indices) * page_size, len(req.origin_input_ids))
        identity = backend.sequence
        backend.sequence += 1
        signals, packets, pools = {}, [], {}
        if len(kv_indices) or last:
            signals["transfer.complete"] = f"{scheduler.group}/pd/{identity}"
        metadata = self.kv_mgr.rows.read(self.aux_index) if last else None
        for target, peer in backend.topology.peers.items():
            destination = backend.reservations[self.bootstrap_room][target]
            if len(destination["pages"][index_slice]) != len(kv_indices):
                raise ValueError("native PD sender chunk exceeds its receiver's page reservation")
            pools[peer["binding"]] = peer["pool"]
            if last:
                packets.append({"target": target, "value": {
                    "kind": "transfer", "room": self.bootstrap_room, "publication": identity, "metadata": metadata,
                }})
        self.signals.update(signals.values())

        # Native Success is published after the whole destination group finishes, not each destination independently.
        if last:
            completion_signals = sorted(self.signals)
            packets.append({"target": backend.local, "value": {"kind": "sender_visible", "room": self.bootstrap_room}})

            # The native transport only notifies after the final chunk; retain all chunks until that notice is consumed.
            backend.published[identity] = {"signals": completion_signals, "pending": set(backend.topology.peers)}
        work = {
            **scheduler.calls.request_work(req, begin, end),
            "pools": pools, "signals": signals,
        }
        parameters = {"transfer_pages": len(kv_indices), "transfer_tokens": end - begin, "last_chunk": int(last)}

        # Register before the call returns: background publication can finish while its enclosing CPU work is still running.
        if last:
            backend.exchange(packets, completion={
                "request": int(req.rid),
                "notification": {"group": scheduler.group, "name": "kv_success",
                                 "work": {"request": scheduler.calls.progress((req,))[0]["id"]}},
                "signals": completion_signals, "after": [],
            })
        with scheduler.calls.call("kv_enqueue", self.send, parameters=parameters, work=work):
            if last:
                scheduler.request_state.note("metadata", "read", [req])
                scheduler.request_state.note("metadata", "write", [req])

    def clear(self):
        super().clear()
        del self.backend.senders[self.bootstrap_room]
        self.backend.reservations.pop(self.bootstrap_room, None)

    def abort(self):
        self.backend.abort(self.bootstrap_room)

    def failure_exception(self):
        raise RuntimeError(self.backend.failures.get(self.bootstrap_room, "native PD sender failed on a peer rank"))


class NativeKvReceiver(NativeKvPolling, BaseKVReceiver):
    def __init__(self, mgr, bootstrap_addr, bootstrap_room=None):
        self.kv_mgr = mgr
        self.backend = mgr.backend
        self.bootstrap_room = bootstrap_room
        self.conclude_state = None
        self.poll_state = None
        self.completions = {}
        self.visible = False
        self.pending = {}
        self.finished = set()
        self.metadata = None
        self.aux_index = None
        self.signals = set()
        self.backend.receivers[bootstrap_room] = self
        mgr.try_ensure_parallel_info(bootstrap_addr)

    def init(self, prefill_dp_rank):
        if prefill_dp_rank != 0:
            raise ValueError("PD control has no data-parallel bootstrap mapping")

    def send_metadata(self, kv_indices, aux_index=None, state_indices=None, decode_prefix_len=None):
        if state_indices:
            raise ValueError("PD transfer template has no auxiliary state-pool contract")
        calls = self.backend.scheduler.calls
        self.backend.scheduler.req_to_metadata_buffer_idx_allocator.bind(self._markov_request, aux_index)
        work = calls.work(None)
        work["context"] = {"request": calls.progress((self._markov_request,))[0]}
        with calls.call("kv_reserve", self.send_metadata, parameters={"transfer_pages": len(kv_indices)}, work=work):
            self.aux_index = aux_index
            pages = kv_indices.tolist()
            request = work["context"]["request"]["id"]
            scheduler, backend = self.backend.scheduler, self.backend
            for index, (peer, target) in enumerate(backend.topology.peers.items()):
                signal = backend.reservation_signal(scheduler.group, request, target["group"])
                self.signals.add(signal)
                packets = [{"target": peer, "value": {
                    "kind": "reservation", "room": self.bootstrap_room, "pages": pages, "prefix": decode_prefix_len or 0,
                }}]
                completion = None
                if scheduler.group == target["publishers"][0]:

                    # One publisher declares the shared gate; every actual publisher supplies its own graph signal.
                    packets.append({"target": peer, "value": {"kind": "reservation_visible", "room": self.bootstrap_room}})
                    completion = {
                        "request": int(self._markov_request.rid),
                        "notification": {"group": target["group"], "name": "kv_ready", "work": {"request": request}},
                        "signals": [backend.reservation_signal(publisher, request, target["group"])
                                    for publisher in target["publishers"]], "after": [],
                    }

                # Payloads can be queued early, but the sender cannot use them until the causal gate is visible.
                backend.exchange(packets, completion=completion)
                publication = calls.request_work(self._markov_request)
                publication["context"]["reservation_peer"] = index
                publication["signals"] = {"reservation.ready": signal}
                with calls.separate("kv_reservation", publication):
                    scheduler.action("kv_reservation", {}, publication)

        # Register before polling: native receive-side publication progresses independently of the next scheduler turn.
        request = work["context"]["request"]["id"]
        self.backend.exchange(
            [{"target": self.backend.local, "value": {"kind": "receiver_visible", "room": self.bootstrap_room}}],
            completion={"request": int(self._markov_request.rid),
                        "notification": {"group": scheduler.group, "name": "kv_success", "work": {"request": request}}, "signals": [], "after": [
                {"group": peer["group"], "name": "kv_success", "work": {"request": request}}
                for peer in self.backend.topology.peers.values()
            ]},
        )

    def _poll(self):
        if self.backend.failed(self.bootstrap_room):
            return KVPoll.Failed
        if self.conclude_state is not None:
            return self.conclude_state
        if self.aux_index is None:
            return KVPoll.WaitingForInput
        acknowledgements = []
        for source, packet in self.pending.items():

            # Delivery already crossed the core completion gate; the receiver only joins native sender notifications.
            if self.metadata is not None and self.metadata != packet["metadata"]:
                raise ValueError("native PD producers disagree on the first-token metadata")
            self.metadata = packet["metadata"]
            self.finished.add(source)
            acknowledgements.append({"target": source, "value": {"kind": "consumed", "publication": packet["publication"]}})
        self.pending.clear()
        if acknowledgements:
            self.backend.exchange(acknowledgements)
        if self.visible and self.finished == self.backend.topology.peers.keys():
            self.kv_mgr.rows.write(self.aux_index, self.metadata)
            self.conclude_state = KVPoll.Success
            return self.conclude_state
        return KVPoll.WaitingForInput

    def clear(self):
        del self.backend.receivers[self.bootstrap_room]

    def abort(self):
        self.backend.abort(self.bootstrap_room)

    def failure_exception(self):
        raise RuntimeError(self.backend.failures.get(self.bootstrap_room, "native PD receiver failed on a peer rank"))
