"""Measure the native scheduler's ingress and token publication with two hooks."""

import ctypes
import time
from pathlib import Path

from markov_adapters.artifacts import write_json
from markov_collection.adapters.frameworks.sglang.content import NativeContent


class NativeLatency:
    def __init__(self, description) -> None:
        self.path = Path(description["instance_directory"]) / "latency.json"
        self.content = NativeContent(description)
        self.requests = {native: {"id": identity} for native, identity in self.content.aliases.items()}
        self.pending = set(self.requests)

    def receive(self, original, receiver):
        from sglang.srt.managers.io_struct import BatchTokenizedGenerateReqInput, TokenizedGenerateReqInput

        messages = original(receiver)
        if not messages:
            return messages
        received_ns = time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)
        for message in messages:
            if isinstance(message, BatchTokenizedGenerateReqInput):
                requests = message
            elif isinstance(message, TokenizedGenerateReqInput):
                requests = (message,)
            else:
                continue
            for request in requests:
                if request.rid in self.requests:
                    self.requests[request.rid].setdefault("ingress_raw_ns", received_ns)
        return messages

    def publish(self, original, sender, output, *args, **kwargs):
        from sglang.srt.managers.io_struct import BatchTokenIDOutput

        result = original(sender, output, *args, **kwargs)
        if sender.socket is None or not isinstance(output, BatchTokenIDOutput) or not output.rids or not self.pending:
            return result

        # Measure the actual scheduler send, before any report serialization.
        published_ns = time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)
        for rid, count, finished in zip(output.rids, output.completion_tokens, output.finished_reasons, strict=True):
            if rid not in self.requests or count <= 0:
                continue
            row = self.requests[rid]
            if rid not in self.pending or row["ingress_raw_ns"] > published_ns:
                raise ValueError(f"invalid native token publication for {rid}")
            row.setdefault("first_publication_raw_ns", published_ns)
            if finished is not None:
                row.update(last_publication_raw_ns=published_ns, output_tokens=count)
                self.pending.remove(rid)

        if not self.pending:
            write_json(self.path, {"clock": "monotonic_raw_ns", "requests": list(self.requests.values())}, replace=False)
        return result

    def install(self) -> None:
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        if getattr(ctypes.CDLL(None), "markov_set_runtime_enabled", None) is not None:
            raise ValueError("latency measurement requires the native observer to be unloaded")
        HookRegistry.register(
            "sglang.srt.managers.scheduler_components.request_receiver.SchedulerRequestReceiver._pull_raw_reqs",
            self.receive, HookType.AROUND,
        )
        HookRegistry.register(
            "sglang.srt.managers.scheduler_components.output_sender.SenderWrapper.send_output",
            self.publish, HookType.AROUND,
        )
        self.content.install()
