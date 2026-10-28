"""Translate simulated arrivals at the native tokenizer receive boundary."""

from array import array

import zmq
from markov_control.adapters.frameworks.sglang.workers import NativeStopped, StopInput
from markov_control.peer import ControlPeer
from sglang.srt.managers.io_struct import TokenizedGenerateReqInput
from sglang.srt.sampling.sampling_params import SamplingParams


class RequestInput:
    def __init__(self, peer: ControlPeer, vocab_size: int) -> None:
        self.peer = peer
        self.vocab_size = vocab_size
        self.arrivals = None

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
        if request is None:

            # End this native receive poll; the next poll waits for core progress.
            self.arrivals = None
            raise zmq.Again()

        if isinstance(request, StopInput):
            return request

        parameters = SamplingParams(max_new_tokens=request["output_tokens"], ignore_eos=True, temperature=0)
        parameters.normalize(None)
        parameters.verify(self.vocab_size)
        return TokenizedGenerateReqInput(
            rid=str(request["request"]),
            input_text="",
            input_ids=array("q", [1] * request["prompt_tokens"]),
            mm_inputs=None,
            sampling_params=parameters,
            return_logprob=False,
            logprob_start_len=-1,
            top_logprobs_num=0,
            token_ids_logprob=None,
            stream=True,
        )


class NativeRequestReceiver:
    def __init__(self, receiver) -> None:
        self.receiver = receiver

    def recv_requests(self):
        requests = self.receiver.recv_requests()
        if any(isinstance(request, StopInput) for request in requests):
            raise NativeStopped()
        return requests
