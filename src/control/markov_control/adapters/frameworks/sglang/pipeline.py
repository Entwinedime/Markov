"""Keep the native PP loop and replace only device work and message transport."""

import torch
from markov_control.adapters.frameworks.sglang.scheduler import NativeScheduler, ResultReady
from markov_control.adapters.frameworks.sglang.workers import NativeStopped, StopInput
from sglang.srt.distributed.parallel_state import P2PWork
from sglang.srt.managers import scheduler_pp_mixin
from sglang.srt.managers.scheduler_components import request_receiver
from sglang.srt.managers.utils import GenerationBatchResult
from sglang.srt.model_executor.forward_batch_info import PPProxyTensors
from torch.utils.weak import WeakIdKeyDictionary


class PipelineWork:
    def __init__(self, peer, work, channel) -> None:
        self.peer = peer
        self.work = work
        self.channel = channel

    def wait(self):
        self.peer.call("wait", **self.work, channel=self.channel)
        if self.channel == "output":

            # The output-ring send is this stage's last transport use, even if its result was processed earlier.
            self.peer.call("release_work", **self.work)


class PipelineScheduler(NativeScheduler):
    def __init__(self, *args, **kwargs) -> None:
        super().__init__(*args, **kwargs)
        self.tensor_work = WeakIdKeyDictionary()
        self.pp_group.send_tensor_dict = self.send_tensors
        self.pp_group.recv_tensor_dict = self.receive_tensors
        scheduler_pp_mixin.point_to_point_pyobj = self.transfer_requests
        request_receiver.point_to_point_pyobj = self.transfer_requests

    def current_lane(self):
        return next(index for index, batch in enumerate(self.running_mbs) if batch is self.running_batch)

    def transfer_requests(self, data, rank, group, src, dst, async_send=False):
        message = {"source": src, "target": dst, "channel": "requests"}
        if rank == src:
            self.peer.exchange("send_object", {**message, "value": data})
            return []
        return self.peer.exchange("receive_object", message)

    def _pp_recv_pyobj_from_prev_stage(self):
        message = super()._pp_recv_pyobj_from_prev_stage()

        # Broadcast termination to this stage's TP peers before leaving a control-ring receive.
        if any(isinstance(item, StopInput) for item in message):
            raise NativeStopped()
        return message

    def tensor(self, geometry, work, channel):
        dtype = getattr(torch, geometry["dtype"].removeprefix("torch."))

        # Hidden states need only geometry; output tokens carry the declared content around the PP ring.
        value = (
            torch.empty(geometry["shape"], dtype=dtype, device="meta")
            if channel == "proxy"
            else torch.tensor(geometry["tokens"], dtype=dtype)
        )
        self.tensor_work[value] = dict(work)
        return value

    def model_result(self, batch, **kwargs):
        response = self.peer.call("model")
        values = response["values"][str(self.graph_rank)]
        if self.pp_group.is_last_rank:
            geometry = {
                **values["sampled_tokens"],
                "tokens": [self.request_input.output_token(req.rid) for req in batch.reqs],
            }
            return GenerationBatchResult(
                logits_output=None,
                next_token_ids=self.tensor(geometry, self.peer.context, "output"),
            )
        tensors = {
            name.removeprefix("proxy."): self.tensor(geometry, self.peer.context, "proxy")
            for name, geometry in values.items()
            if name.startswith("proxy.")
        }
        return GenerationBatchResult(pp_hidden_states_proxy_tensors=PPProxyTensors(tensors))

    def send_tensors(self, tensor_dict, all_gather_group=None, async_send=True):
        channel = tensor_dict["__msg_type__"]
        tensors = {name: value for name, value in tensor_dict.items() if name != "__msg_type__"}
        work = self.tensor_work[next(iter(tensors.values()))]
        if any(self.tensor_work[value] != work for value in tensors.values()):
            raise ValueError("Native PP message mixes different work identities")
        self.peer.call("send", **work, channel=channel)
        packet = {
            "work": work,
            "channel": channel,
            "values": {
                name: {
                    "shape": list(value.shape),
                    "dtype": str(value.dtype),
                    **({"tokens": value.tolist()} if channel == "output" else {}),
                }
                for name, value in tensors.items()
            },
        }
        self.peer.exchange(
            "send_object",
            {
                "source": self.control_rank,
                "target": self.pp_group.next_rank,
                "channel": "tensors",
                "value": packet,
            },
        )
        return [P2PWork(PipelineWork(self.peer, work, channel), None)]

    def receive_tensors(self, all_gather_group=None):
        packet = self.peer.exchange(
            "receive_object",
            {
                "source": self.pp_group.prev_rank,
                "target": self.control_rank,
                "channel": "tensors",
            },
        )
        self.peer.call("receive", **packet["work"], channel=packet["channel"])
        tensors = {
            name: self.tensor(geometry, packet["work"], packet["channel"])
            for name, geometry in packet["values"].items()
        }
        return {**tensors, "__msg_type__": packet["channel"]}

    def _pp_send_recv_and_preprocess_output_tensors(self, next_first_rank_mb_id, next_mb_id, mbs, *args):
        outputs, result, event, work = super()._pp_send_recv_and_preprocess_output_tensors(
            next_first_rank_mb_id, next_mb_id, mbs, *args
        )
        if event is not None:

            # Preparation precedes other native work; the event consumer waits later.
            self.peer.call("prepare_result", **mbs[next_mb_id].control_work)
            event = ResultReady(self.peer, mbs[next_mb_id].control_work)
        return outputs, result, event, work

    def run_event_loop(self):
        try:
            super().run_event_loop()
        except NativeStopped:

            # The native loop is persistent; shutdown still consumes its outstanding send handles.
            for work in (self.send_req_work, self.send_proxy_work, self.send_output_work):
                self._pp_commit_comm_work(work)
