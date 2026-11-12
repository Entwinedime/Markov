"""Translate simulated arrivals at the native tokenizer receive boundary."""

from array import array
import sys

import zmq
from markov_adapters.sglang.pipeline import PipelineABI
from markov_control.peer import ControlPeer
from markov_workload import WorkloadContent
from markov_workload.routing import RoutedContent
from sglang.srt.managers.io_struct import TokenizedGenerateReqInput
from sglang.srt.sampling.sampling_params import SamplingParams


class RequestInput:
    def __init__(
        self, peer: ControlPeer, vocab_size: int, workload: dict, routes, mode: str, expert_layers: set[int]
    ) -> None:
        self.peer = peer
        self.vocab_size = vocab_size
        self.mode = mode
        self.arrivals = None
        self.waiting = False
        self.context = None
        self.in_flow = False
        self.requests = workload["requests"]
        self.content = WorkloadContent(self.requests, workload["prefixes"])
        self.content.check_vocabulary(vocab_size)
        self.routing = RoutedContent(self.content, routes["assignments"], expert_layers) if routes is not None else None
        if self.routing is not None:
            from sglang.srt.plugins.hook_registry import HookRegistry, HookType

            HookRegistry.register("sglang.srt.mem_cache.radix_cache.RadixKey.__init__", self.cache_key, HookType.AROUND)

    def cache_key(self, original, key, token_ids, extra_key=None, is_bigram=False, limit=None):
        if extra_key is not None:
            token_ids = self.routing.key(extra_key, token_ids if limit is None else token_ids[:limit])
            extra_key = None
        original(key, token_ids, extra_key, is_bigram, limit)

    def recv_pyobj(self, flags):
        if self.arrivals is None:
            advance = self.peer.call("input", waiting=self.waiting, context=self.context, in_flow=self.in_flow)
            if advance["kind"] != "advance":
                raise ValueError("Native control expected an advance boundary")
            self.arrivals = iter(advance["arrivals"])

        request = next(self.arrivals, None)

        # End this native receive poll; the next poll waits for core progress.
        if request is None:
            self.arrivals = None
            raise zmq.Again()

        identity = request
        request = self.requests[identity]
        parameters = SamplingParams(max_new_tokens=request["output_tokens"], ignore_eos=True, temperature=0)
        parameters.normalize(None)
        parameters.verify(self.vocab_size)
        return TokenizedGenerateReqInput(
            rid=str(identity),
            input_text="",
            input_ids=array("q", self.content.prompt(request["id"])),
            input_embeds=None,
            mm_inputs=None,
            token_type_ids=None,
            sampling_params=parameters,
            return_logprob=False,
            logprob_start_len=-1,
            top_logprobs_num=0,
            token_ids_logprob=None,
            stream=True,
            extra_key=request["id"] if self.routing is not None else None,
            bootstrap_host="markov-prefill" if self.mode != "null" else None,
            bootstrap_room=identity + 1 if self.mode != "null" else None,
        )

    def output_token(self, identity: str) -> int:
        return self.content.output_token(self.requests[int(identity)]["id"])


class NativeRequestReceiver:
    def __init__(self, receiver, scheduler) -> None:
        self.receiver = receiver
        self.scheduler = scheduler
        self.round_started = False
        self.model_ran = False
        self.idle_reported = False

    def recv_requests(self):
        scheduler = self.scheduler

        # The native loop owns this cursor. A PP round includes every slot, not each individual receive.
        first_slot = scheduler.ps.pp_size == 1 or sys._getframe(1).f_locals["mb_id"] == 0
        scheduler.request_input.waiting = False
        if first_slot:

            # Native on_idle can precede round-tail work; park only after that work reaches the next receive.
            idle = self.idle_reported and scheduler.is_fully_idle() and not PipelineABI.pending_model_sends(scheduler)
            self.idle_reported = False
            if idle:
                if scheduler.ps.pp_rank == 0 and scheduler.ps.tp_rank == 0:
                    scheduler.peer.call("idle", waiting=True)
                else:
                    scheduler.peer.send({"kind": "idle", "waiting": False})

            # Pending transfers can bypass on_idle; completed rounds must still have a safe external wait.
            scheduler.request_input.waiting = (
                self.round_started and not self.model_ran
                and not scheduler.waiting_queue and scheduler.chunked_req is None
                and scheduler.pd is not None and scheduler.pd.waiting()
            )
            self.round_started = True
            self.model_ran = False
        scheduler.request_input.context = scheduler.calls.scheduler_work()["context"]
        scheduler.request_input.in_flow = scheduler.calls.started
        return self.receiver.recv_requests()
