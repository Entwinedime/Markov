"""Measure native scheduler ingress and token publication without full profiling."""

import ctypes
import time
from pathlib import Path

from markov_adapters.artifacts import write_json
from markov_collection.adapters.frameworks.sglang.content import NativeContent
from markov_collection.adapters.frameworks.sglang.window import ObservationWindow
from markov_collection.adapters.runtimes.ascend.host_profile import AscendSpecializations


class NativeLatency:
    def __init__(self, description) -> None:
        self.path = Path(description["instance_directory"]) / "latency.json"
        self.content = NativeContent(description)
        self.requests = {native: {"id": identity} for native, identity in self.content.aliases.items()}
        self.pending = set(self.requests)
        self.host = None
        self.window = None
        self.specializations = None
        if description["host_timing"]:
            from markov_collection.adapters.frameworks.sglang.host_samples import NativeHostSamples

            self.rank, = description["native_participants"]
            self.host = NativeHostSamples(self.content.aliases, self.rank, self.path.parent)
            self.content.auxiliary = self.host.auxiliary
            self.window = ObservationWindow(Path(description["output"]), description)
        else:
            self.specializations = AscendSpecializations()

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
                    if self.specializations is not None and not self.specializations.started:
                        self.specializations.start()
                    self.requests[request.rid].setdefault("ingress_raw_ns", received_ns)
        return messages

    def request(self, original, scheduler, request):
        self.window.begin(request.rid)
        if request.rid in self.requests:
            self.host.enabled = True
        return original(scheduler, request)

    def write_latency(self):
        report = {"clock": "monotonic_raw_ns", "requests": list(self.requests.values())}
        if self.specializations is not None:
            report["specialization_loads"] = self.specializations.finish()
        write_json(self.path, report, replace=False)

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

        if not self.pending and self.host is None:
            self.write_latency()
        return result

    def flush(self, original, scheduler, *args, **kwargs):
        result = original(scheduler, *args, **kwargs)
        if result and not self.host.profile.started:
            self.host.scheduler = scheduler
            self.host.profile.start()
        elif result and not self.pending:

            # Flush follows terminal publication on the same scheduler, including the emitter's caller return.
            self.host.enabled = False
            write_json(self.path.with_name("host-samples.json"), self.host.document(), replace=False)
            self.write_latency()
        return result

    def idle(self, original, scheduler):
        if scheduler.is_fully_idle():
            self.window.group_idle(self.rank)
            if self.window.inputs_complete() and not self.pending:
                self.host.enabled = False
        return original(scheduler)

    def install(self) -> None:
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        if self.host is None and getattr(ctypes.CDLL(None), "markov_set_runtime_enabled", None) is not None:
            raise ValueError("latency measurement requires the native observer to be unloaded")
        HookRegistry.register(
            "sglang.srt.managers.scheduler_components.request_receiver.SchedulerRequestReceiver._pull_raw_reqs",
            self.receive, HookType.AROUND,
        )
        if self.host is not None:
            self.host.install()
            HookRegistry.register("sglang.srt.managers.scheduler.Scheduler.handle_generate_request", self.request, HookType.AROUND)
            HookRegistry.register("sglang.srt.managers.scheduler.Scheduler.flush_cache", self.flush, HookType.AROUND)
            HookRegistry.register("sglang.srt.managers.scheduler.Scheduler.on_idle", self.idle, HookType.AROUND)
        self.content.install()
        HookRegistry.register(
            "sglang.srt.managers.scheduler_components.output_sender.SenderWrapper.send_output",
            self.publish, HookType.AROUND,
        )
