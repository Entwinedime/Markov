"""Use native worker idle and reset boundaries for lightweight serving measurements."""

import ctypes
from pathlib import Path
from time import CLOCK_MONOTONIC_RAW, clock_gettime_ns

from markov_adapters.artifacts import write_json
from markov_collection.adapters.frameworks.sglang.content import NativeContent
from markov_collection.adapters.frameworks.sglang.host_samples import NativeHostSamples
from markov_collection.adapters.frameworks.sglang.window import ObservationWindow
from markov_collection.adapters.runtimes.ascend.host_profile import AscendSpecializations


class NativeTiming:
    def __init__(self, description):
        self.directory = Path(description["instance_directory"]) / "timing"
        self.rank_offset = description["rank_offset"]
        self.window = ObservationWindow(Path(description["output"]), description)
        self.content = NativeContent(description)
        self.host = NativeHostSamples(self.content.aliases) if description["host_timing"] else None
        if self.host is not None:
            self.content.auxiliary = self.host.auxiliary
        else:
            self.specializations = AscendSpecializations()
            self.requests = {native: {"id": identity} for native, identity in self.content.aliases.items()}

    def rank(self, scheduler):
        coordinates = scheduler.ps
        return self.rank_offset + coordinates.pp_rank * coordinates.tp_size + coordinates.tp_rank

    def request(self, original, scheduler, request):
        self.window.begin(request.rid)
        if self.host is not None and request.rid in self.window.requests["source"]:
            self.host.enabled = True
        return original(scheduler, request)

    def receive(self, original, receiver):
        from sglang.srt.managers.io_struct import BatchTokenizedGenerateReqInput, TokenizedGenerateReqInput

        messages = original(receiver)
        if not messages:
            return messages
        received_ns = clock_gettime_ns(CLOCK_MONOTONIC_RAW)
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
        if sender.socket is None or not isinstance(output, BatchTokenIDOutput) or not output.rids:
            return result

        # Only the actual sending worker has a socket. Record before any serialization of measurements.
        published_ns = clock_gettime_ns(CLOCK_MONOTONIC_RAW)
        for rid, count, finished in zip(output.rids, output.completion_tokens, output.finished_reasons, strict=True):
            if rid not in self.requests or count <= 0:
                continue
            row = self.requests[rid]
            if "output_tokens" in row or row["ingress_raw_ns"] > published_ns:
                raise ValueError(f"invalid native token publication for {rid}")
            row.setdefault("first_publication_raw_ns", published_ns)
            if finished is not None:
                row.update(last_publication_raw_ns=published_ns, output_tokens=count)
        return result

    def flush(self, original, scheduler, empty_cache=True):
        result = original(scheduler, empty_cache=False)
        if not result or not self.window.drained:
            return result
        rank = self.rank(scheduler)
        directory = self.directory / f"rank-{rank}"
        if self.window.phase == "warmup":
            directory.mkdir(parents=True)
            if self.host is not None:
                self.host.start(scheduler, rank, directory)
            else:
                self.specializations.start()
        else:
            if self.host is not None:
                record = self.host.document()
            else:
                requests = [row for row in self.requests.values() if "first_publication_raw_ns" in row]
                if requests and (len(requests) != len(self.requests) or any("output_tokens" not in row for row in requests)):
                    raise ValueError("native output worker did not publish complete request latencies")
                record = {"requests": requests, "specialization_loads": self.specializations.finish()}
            write_json(directory / "record.json", {**record, "rank": rank}, replace=False)

        self.window.prepared_phase = self.window.phase
        return result

    def idle(self, original, scheduler):
        if scheduler.is_fully_idle():
            rank = self.rank(scheduler)
            self.window.group_idle(rank)
            if self.window.inputs_complete() and not self.window.drained:
                if self.host is not None:
                    self.host.enabled = False
                self.window.drain(rank)
            if self.window.prepared_phase is not None:
                self.window.complete(rank, scheduler.idle_sleeper is not None)
        return original(scheduler)

    def install(self):
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        if self.host is not None:
            self.host.install()
        else:
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
        HookRegistry.register("sglang.srt.managers.scheduler.Scheduler.handle_generate_request", self.request, HookType.AROUND)
        HookRegistry.register("sglang.srt.managers.scheduler.Scheduler.flush_cache", self.flush, HookType.AROUND)
        HookRegistry.register("sglang.srt.managers.scheduler.Scheduler.on_idle", self.idle, HookType.AROUND)
