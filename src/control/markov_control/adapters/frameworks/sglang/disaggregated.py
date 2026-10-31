"""Keep native PD admission and result processing while using core resources and transport."""

from dataclasses import dataclass, fields
from types import SimpleNamespace

import torch
from markov_control.adapters.frameworks.sglang.batches import NativeBatch
from markov_control.adapters.frameworks.sglang.pipeline import PipelineScheduler
from markov_control.adapters.frameworks.sglang.scheduler import NativeScheduler
from sglang.srt.disaggregation import decode, prefill
from sglang.srt.disaggregation.base.conn import BaseKVReceiver, BaseKVSender, KVArgs, KVPoll, KVTransferMetric
from sglang.srt.disaggregation.utils import KVClassType, setup_state_kv_args
from sglang.srt.managers import scheduler as scheduler_module
from sglang.srt.managers.scheduler_components.batch_result_processor import SchedulerBatchResultProcessor


class GraphTransport:
    def __init__(self, queue):
        self.peer = queue.scheduler.peer
        self.buffers = queue.metadata_buffers
        self.kv_args = KVArgs()
        setup_state_kv_args(
            self.kv_args,
            queue.token_to_kv_pool,
            queue.draft_token_to_kv_pool,
            queue.scheduler.model_config.num_hidden_layers,
            req_to_token_pool=queue.scheduler.req_to_token_pool,
        )
        port = queue.scheduler.server_args.disaggregation_bootstrap_port
        self.prefill_info_table = {f"markov-prefill:{port}": SimpleNamespace(dp_size=1, follow_bootstrap_room=False)}


class GraphSender(BaseKVSender):
    def __init__(self, mgr, bootstrap_addr, bootstrap_room, dest_tp_ranks, pp_rank):
        self.manager = mgr
        self.request = bootstrap_room - 1
        self.sent = False
        self.conclude_state = None
        mgr.peer.call("transfer_sender", request=self.request)

    def init(self, num_kv_indices, aux_index=None):
        self.index = aux_index

    def should_send_kv_chunk(self, num_pages, last_chunk):
        self.final = last_chunk
        return super().should_send_kv_chunk(num_pages, last_chunk)

    def send(self, kv_indices, state_indices=None):
        payload = (
            {
                name: value[self.index].tolist()
                for name, value in vars(self.manager.buffers).items()
                if isinstance(value, torch.Tensor)
            }
            if self.final
            else None
        )
        self.manager.peer.call(
            "transfer_submit",
            request=self.request,
            pages=(kv_indices - 1).tolist(),
            final=self.final,
            metadata_slot=self.index,
            payload=payload,
        )
        self.sent = True

    def poll(self):
        if self.conclude_state is not None:
            return self.conclude_state
        state = self.manager.peer.call("transfer_poll", request=self.request)["state"]
        if state == "completed":
            self.conclude_state = KVPoll.Success
            return self.conclude_state
        if self.sent:
            return KVPoll.Transferring
        return KVPoll.WaitingForInput if state == "ready" else KVPoll.Bootstrapping

    def get_transfer_metric(self):
        return KVTransferMetric()

    def failure_exception(self):
        raise RuntimeError("Core KV transfer failed")


class GraphReceiver(BaseKVReceiver):
    def __init__(self, mgr, bootstrap_addr, bootstrap_room=None):
        self.manager = mgr
        self.request = bootstrap_room - 1
        self.index = None
        self.conclude_state = None
        self.require_staging = False
        self.initialized = False

    def init(self, prefill_dp_rank):
        self.manager.peer.call("transfer_receiver", request=self.request)
        self.initialized = True

    def send_metadata(self, kv_indices, aux_index=None, state_indices=None, decode_prefix_len=None):
        self.index = aux_index

    def poll(self):
        if self.conclude_state is not None:
            return self.conclude_state
        if not self.initialized:
            return KVPoll.Bootstrapping
        result = self.manager.peer.call("transfer_poll", request=self.request)
        if result["state"] == "bootstrapping":
            return KVPoll.Bootstrapping
        if self.index is None:
            return KVPoll.WaitingForInput
        if result["state"] != "completed":
            return KVPoll.Transferring

        # Metadata enters native CPU views only after the simulated copy has completed.
        for name, values in result["payload"].items():
            tensor = getattr(self.manager.buffers, name)
            tensor[self.index] = torch.tensor(values, dtype=tensor.dtype)
        self.conclude_state = KVPoll.Success
        return self.conclude_state

    def failure_exception(self):
        raise RuntimeError("Core KV receive failed")


def transfer_class(backend, kind):
    return {KVClassType.SENDER: GraphSender, KVClassType.RECEIVER: GraphReceiver}[kind]


class PrefillQueue(prefill.PrefillBootstrapQueue):
    def _init_kv_manager(self):
        return GraphTransport(self)


class ReservationQueue(decode.DecodePreallocQueue):
    def _init_kv_manager(self):
        return GraphTransport(self)

    def _pre_alloc(self, req, *args, **kwargs):
        allocator = self.token_to_kv_pool_allocator
        allocator.reservation = req
        try:
            indices = super()._pre_alloc(req, *args, **kwargs)
            self.scheduler.peer.call("reserve", request=int(req.rid))
            return indices
        finally:
            allocator.reservation = None


class TransferQueue(decode.DecodeTransferQueue):
    def _commit_transfer_to_req(self, request):
        self.scheduler.peer.call("received", request=int(request.req.rid), metadata_slot=request.metadata_buffer_index)
        return super()._commit_transfer_to_req(request)


@dataclass(kw_only=True, frozen=True)
class PrebuiltResults(SchedulerBatchResultProcessor):
    controller: object

    def process_batch_result_prebuilt(self, batch):
        self.controller.published.identities.clear()
        super().process_batch_result_prebuilt(batch)
        self.controller.published.complete(self.controller.peer, batch, "accept")
        self.controller.peer.call("release_work")

        # The following model batch is new work even when it includes these prebuilt members.
        self.controller.begin_iteration()


class DisaggregatedScheduler(NativeScheduler):
    def __init__(self, *args, **kwargs):
        prefill.get_kv_class = transfer_class
        decode.get_kv_class = transfer_class
        decode.ScheduleBatch = NativeBatch
        scheduler_module.PrefillBootstrapQueue = PrefillQueue
        scheduler_module.DecodePreallocQueue = ReservationQueue
        scheduler_module.DecodeTransferQueue = TransferQueue
        super().__init__(*args, **kwargs)
        processor = self.batch_result_processor
        self.batch_result_processor = PrebuiltResults(
            **{field.name: getattr(processor, field.name) for field in fields(processor)}, controller=self
        )

    def process_input_requests(self, requests):

        # PD performs allocation and polling before selecting a model batch.
        self.begin_iteration()
        return super().process_input_requests(requests)

    def get_next_disagg_prefill_batch_to_run(self):
        return self.prepare(super().get_next_disagg_prefill_batch_to_run())

    def get_next_disagg_decode_batch_to_run(self):
        return self.prepare(super().get_next_disagg_decode_batch_to_run())

    def process_prefill_chunk(self):

        # Both PD loops filter the preceding Prefill here, including unchanged membership.
        with NativeBatch.report_selection(self.last_batch, "previous"):
            return super().process_prefill_chunk()


class PipelineDisaggregatedScheduler(DisaggregatedScheduler, PipelineScheduler):
    def get_new_batch_prefill(self):

        # The PP prefill loop selects here, bypassing the single-stage disaggregated entry point.
        return self.prepare(super().get_new_batch_prefill())
