"""Translate simulated arrivals at the native tokenizer receive boundary."""

from array import array

import zmq
from markov_control.adapters.frameworks.sglang.workers import NativeStopped, StopInput
from markov_control.peer import ControlPeer
from markov_workload import WorkloadContent
from markov_workload.routing import RoutedContent
from sglang.srt.managers.io_struct import TokenizedGenerateReqInput
from sglang.srt.sampling.sampling_params import SamplingParams


class RequestInput:
    def __init__(
        self, peer: ControlPeer, vocab_size: int, bootstrap: tuple[str, int] | None, workload: dict, routes
    ) -> None:
        self.peer = peer
        self.vocab_size = vocab_size
        self.bootstrap = bootstrap
        self.arrivals = None
        self.requests = workload["requests"]
        self.content = WorkloadContent(self.requests, workload["prefixes"])
        self.content.check_vocabulary(vocab_size)
        self.routing = RoutedContent(self.content, routes["assignments"]) if routes is not None else None
        if self.routing is not None:
            from sglang.srt.plugins.hook_registry import HookRegistry, HookType

            HookRegistry.register("sglang.srt.mem_cache.radix_cache.RadixKey.__init__", self.cache_key, HookType.AROUND)
            HookRegistry.apply_hooks()

    def cache_key(self, original, key, token_ids, extra_key=None, is_bigram=False, limit=None):
        if extra_key is not None:
            token_ids = self.routing.key(extra_key, token_ids if limit is None else token_ids[:limit])
            extra_key = None
        original(key, token_ids, extra_key, is_bigram, limit)

    def recv_pyobj(self, flags):
        if self.arrivals is None:
            advance = self.peer.receive()
            if advance["kind"] == "stop":
                self.arrivals = iter([StopInput()])
            elif advance["kind"] == "advance":
                self.arrivals = iter(advance["arrivals"])
            else:
                raise ValueError("Native control expected an advance boundary")

        request = next(self.arrivals, None)

        # End this native receive poll; the next poll waits for core progress.
        if request is None:
            self.arrivals = None
            raise zmq.Again()

        if isinstance(request, StopInput):
            return request

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
            bootstrap_host=self.bootstrap[0] if self.bootstrap else None,
            bootstrap_port=self.bootstrap[1] if self.bootstrap else None,
            bootstrap_room=identity + 1 if self.bootstrap else None,
            extra_key=request["id"] if self.routing is not None else None,
        )

    def output_token(self, identity: str) -> int:
        return self.content.output_token(self.requests[int(identity)]["id"])


class NativeRequestReceiver:
    def __init__(self, receiver) -> None:
        self.receiver = receiver

    def recv_requests(self):
        requests = self.receiver.recv_requests()
        if any(isinstance(request, StopInput) for request in requests):
            raise NativeStopped()
        return requests
