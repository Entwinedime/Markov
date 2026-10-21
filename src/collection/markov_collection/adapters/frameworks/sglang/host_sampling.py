"""Observe native batch provenance for independent host measurements."""

from __future__ import annotations

import atexit
import inspect
import json
import os
import sys
import time
from collections.abc import Callable, Iterator
from contextlib import AbstractContextManager, contextmanager, nullcontext
from pathlib import Path
from typing import Any

from markov_collection.adapters.frameworks.sglang.batch_observer import SGLangBatchObserver
from markov_collection.adapters.frameworks.sglang.input_broadcast import SGLangInputBroadcast
from markov_collection.adapters.runtimes.ascend.environment import RuntimeEnvironment
from markov_collection.adapters.runtimes.ascend.transfer_sampling import TransferSampling
from markov_collection.artifacts.json_file import write_json
from markov_collection.instrumentation.cpu_sampling import CpuFunction, CpuSampling
from markov_collection.instrumentation.native_regions import CpuNativeRegions, NativeRegionWrapper


class SGLangHostSampling(SGLangBatchObserver):
    def __init__(
        self,
        directory: Path,
        request_ids: set[str],
        control_ids: set[str],
        regions: dict[str, Any],
        *,
        cpu: bool,
        source_rank: int | None = None,
    ) -> None:
        super().__init__(request_ids, source_rank=source_rank)
        self.directory = directory
        self._sampling = TransferSampling(directory.parent / "cpu_workers")
        self._cpu = CpuSampling(directory.parent / "cpu_activities") if cpu else None
        self._regions = CpuNativeRegions(regions) if cpu else None
        self._control_ids = control_ids
        self._cleaned_ids: set[str] = set()
        self._sequence = 0
        self._rank: int | None = None
        self._records: list[dict[str, Any]] = []
        self._scopes: list[dict[str, Any]] = []
        self._scheduler: Any = None
        self._schedule_context: Any = None
        self._pending_schedule: dict[str, Any] | None = None
        self._awaiting_idle = False
        atexit.register(self.close)

    def _observer(self) -> AbstractContextManager[None]:
        return self._cpu.observer() if self._cpu is not None else nullcontext()

    def _control(self, requests: list[Any]) -> bool:
        return any(request.rid in self._control_ids and request.rid not in self._cleaned_ids for request in requests)

    @contextmanager
    def _scope(
        self, step: str, metadata: dict[str, Any] | None, *, control: bool = False, pending: bool = False
    ) -> Iterator[dict[str, Any]]:
        with self._observer():
            self._sequence += 1
            record = {
                "scope_id": self._sequence,
                "pid": os.getpid(),
                "rank": self._rank,
                "step": step,
                "metadata": metadata,
                "completed": False,
                "control": control,
                "cpu_recording": self._cpu is not None and not control,
                "work_contexts": [],
                "control_interval": [[self._sequence, 0], [self._sequence, 1]],
                "_retain": not pending,
            }
            self._scopes.append(record)
        try:
            with self._sampling.scope(0 if control else self._sequence):
                scope = (
                    self._cpu.scope(self._sequence, "schedule_pending" if pending else step)
                    if self._cpu is not None and not control
                    else nullcontext()
                )
                with scope:
                    with self._observer():
                        record["entered_ns"] = time.perf_counter_ns()
                    yield record
                    with self._observer():
                        record["completed"] = True
        finally:
            with self._observer():
                self._scopes.pop()
                if record.pop("_retain") and (self._cpu is not None or record["metadata"] is not None):
                    self._records.append(record)

    def receive(self, original: Callable[..., Any], receiver: Any, *args: Any, **kwargs: Any) -> Any:
        with self._observer():
            if self._scheduler is None or self._cleaned_ids == self.request_ids:
                active = False
            else:
                visible = self._visible_requests(self._scheduler)
                self._rank = self.source_rank(receiver.ps.tp_rank)
                self._schedule_context = self._scope("schedule", None, control=self._control(visible), pending=True)
                active = True
        if not active:
            return original(receiver, *args, **kwargs)
        self._pending_schedule = self._schedule_context.__enter__()
        with self._observer():
            self._pending_schedule.update(input_requests=[], input_supported=True)
            self._pending_schedule["_retain"] = any(
                req.rid in self.request_ids and req.rid not in self._cleaned_ids for req in visible
            )
        try:
            result = original(receiver, *args, **kwargs)
            with self._observer():
                incoming, supported = self._input_requests(result)
                record = self._pending_schedule
                record["input_requests"] = incoming
                record["input_supported"] = supported
                for context in record["work_contexts"]:
                    if context is None or "message" not in context:
                        continue
                    message = context.pop("message")
                    # The completed native receive supplies its logical cohort;
                    # tensor extent remains the actual encoded observation.
                    context["collective"] = {
                        "operation": "broadcast",
                        "ranks": message["group_ranks"],
                        "input_shape": [],
                        "data_type": message["data_type"],
                        "host_message": {
                            "root": message["root"],
                            "wire_bytes": message["wire_bytes"],
                            "incoming_requests": [
                                [request["prompt_tokens"], request["output_tokens"]] for request in incoming
                            ],
                        },
                    }
                record["_retain"] |= any(item["id"] in self.request_ids for item in incoming)
                record["control"] |= any(item["id"] in self._control_ids for item in incoming)
            return result
        except BaseException:
            self._finish_schedule(sys.exc_info())
            raise

    def process(self, original: Callable[..., Any], scheduler: Any, *args: Any, **kwargs: Any) -> Any:
        self._scheduler = scheduler
        try:
            result = original(scheduler, *args, **kwargs)
            with self._observer():
                if self._pending_schedule is not None:
                    self._pending_schedule["input_supported"] &= self._inputs_accepted(
                        scheduler, self._pending_schedule["input_requests"]
                    )
            return result
        except BaseException:
            self._finish_schedule(sys.exc_info())
            raise

    def schedule(self, original: Callable[..., Any], scheduler: Any, *args: Any, **kwargs: Any) -> Any:
        if self._pending_schedule is None:
            return original(scheduler, *args, **kwargs)
        with self._observer():
            record = self._pending_schedule
            visible = self._visible_requests(scheduler)
            previous = scheduler.last_batch
            pending = previous is not None and previous.forward_mode.is_extend()
            record["scheduling"] = {
                "transition": {
                    "running_requests": len(scheduler.running_batch.reqs),
                    "pending_prefill_requests": len(previous.reqs) if pending else None,
                    "admission_blocked": bool(scheduler.running_batch.batch_is_full),
                },
                "waiting_requests": len(scheduler.waiting_queue),
                "filters": [],
                "merge": None,
                "incoming_requests": [
                    [item["prompt_tokens"], item["output_tokens"]] for item in record["input_requests"]
                ],
            }
        try:
            batch = original(scheduler, *args, **kwargs)
            at_ns = time.perf_counter_ns()
            with self._observer():
                record["native_return_ns"] = at_ns
                if batch is not None:
                    phase = "prefill" if batch.forward_mode.is_extend() else "decode"
                    record["metadata"] = self._batch(batch, phase)
                else:
                    # An empty decision handles no model batch. Associate it
                    # with the live sampling cohort without inventing members.
                    record["metadata"] = {
                        "phase": "idle",
                        "requests": [],
                        "scope_request_ids": [
                            req.rid
                            for req in visible
                            if req.rid in self.request_ids and req.rid not in self._cleaned_ids
                        ],
                    }
                    self._awaiting_idle = True
            if batch is not None:
                self._finish_schedule()
            return batch
        except BaseException:
            self._finish_schedule(sys.exc_info())
            raise

    def idle(self, original: Callable[..., Any], scheduler: Any, *args: Any, **kwargs: Any) -> Any:
        try:
            result = original(scheduler, *args, **kwargs)
            at_ns = time.perf_counter_ns()
            if self._awaiting_idle:
                with self._observer():
                    record = self._pending_schedule
                    record["native_return_ns"] = at_ns
                    members = record["metadata"]["scope_request_ids"]
                    cleanup = (
                        bool(members)
                        and record["input_supported"]
                        and all(identity in self._finished_ids for identity in members)
                    )
                self._finish_schedule()
                if cleanup:
                    with self._observer():
                        # An observed empty decision and its native idle suffix
                        # establish cleanup; a finished response alone does not.
                        self.close()
                        self._cleaned_ids = self._finished_ids.copy()
                        write_json(
                            self.directory / f"rank-{self._rank}-cleanup.json",
                            {
                                "rank": self._rank,
                                "scope_id": record["scope_id"],
                                "completion": "native_empty_schedule",
                                "request_ids": sorted(self._cleaned_ids),
                            },
                        )
            return result
        except BaseException:
            self._finish_schedule(sys.exc_info())
            raise

    def _finish_schedule(self, error: tuple[Any, Any, Any] = (None, None, None)) -> None:
        context = self._schedule_context
        self._schedule_context = None
        self._pending_schedule = None
        self._awaiting_idle = False
        if context is not None:
            context.__exit__(*error)

    def filter(self, original: Callable[..., Any], batch: Any, *args: Any, **kwargs: Any) -> Any:
        with self._observer():
            owner = self._scopes[-1] if self._scopes and self._scopes[-1]["step"] == "schedule" else None
            before = len(batch.reqs)
        result = original(batch, *args, **kwargs)
        if owner is not None:
            with self._observer():
                owner["scheduling"]["filters"].append([before, len(batch.reqs)])
        return result

    def merge(self, original: Callable[..., Any], batch: Any, other: Any, *args: Any, **kwargs: Any) -> Any:
        with self._observer():
            if self._scopes and self._scopes[-1]["step"] == "schedule":
                self._scopes[-1]["scheduling"]["merge"] = {
                    "left_count": len(batch.reqs),
                    "right_count": len(other.reqs),
                }
        return original(batch, other, *args, **kwargs)

    def prepare_prefill(self, original: Callable[..., Any], batch: Any, *args: Any, **kwargs: Any) -> Any:
        return self._prepare(original, batch, "prefill", *args, **kwargs)

    def prepare_decode(self, original: Callable[..., Any], batch: Any, *args: Any, **kwargs: Any) -> Any:
        return self._prepare(original, batch, "decode", *args, **kwargs)

    def _prepare(self, original: Callable[..., Any], batch: Any, phase: str, *args: Any, **kwargs: Any) -> Any:
        with self._observer():
            tracked = self._tracked(batch.reqs)
        if not tracked:
            return original(batch, *args, **kwargs)

        with self._observer():
            self._batch_sequence += 1
            self._batch_ids[id(batch)] = self._batch_sequence
            metadata = self._batch(batch, phase)
            control = self._control(batch.reqs)
            if self._regions is not None:
                self._regions.install_components(batch, self._cpu, "batch")
        with self._scope("prepare", metadata, control=control) as record:
            with self._observer():
                # SGLang selects the batch before entering Prepare; Prepare
                # owns the scheduler suffix through the parent's native return.
                if len(self._scopes) > 1 and self._scopes[-2]["step"] == "schedule":
                    parent = self._scopes[-2]
                    record["control_interval"][1] = [parent["scope_id"], 1]
                    parent["control_interval"][1] = [record["scope_id"], 0]
            result = original(batch, *args, **kwargs)
            with self._observer():
                record["native_return_ns"] = time.perf_counter_ns()
            return result

    def execute(self, original: Callable[..., Any], scheduler: Any, batch: Any, *args: Any, **kwargs: Any) -> Any:
        if not self._tracked(batch.reqs):
            return original(scheduler, batch, *args, **kwargs)

        phase = "prefill" if batch.forward_mode.is_extend() else "decode"
        with self._scope(phase, self._batch(batch, phase), control=self._control(batch.reqs)) as record:
            result = original(scheduler, batch, *args, **kwargs)
            with self._observer():
                record["native_return_ns"] = time.perf_counter_ns()
            return result

    def publish(self, original: Callable[..., Any], scheduler: Any, batch: Any, *args: Any, **kwargs: Any) -> Any:
        if not self._tracked(batch.reqs):
            return original(scheduler, batch, *args, **kwargs)

        phase = "prefill" if batch.forward_mode.is_extend() else "decode"
        with self._observer():
            metadata = self._batch(batch, phase)
            output_before = [
                (req.send_token_offset, req.read_offset is None or req.surr_offset is None) for req in batch.reqs
            ]
        with self._scope("publish", metadata, control=self._control(batch.reqs)) as record:
            result = original(scheduler, batch, *args, **kwargs)
            with self._observer():
                record["native_return_ns"] = time.perf_counter_ns()
                finished = [req.rid for req in batch.reqs if req.finished()]
                metadata["finished_request_ids"] = finished
                finished_ids = set(finished)
                # Read native offsets after output packing; no payload copy or
                # predicted policy substitutes for the actual work observed.
                metadata["publication"] = {
                    "requests": [
                        {
                            "output_tokens": req.send_token_offset - sent,
                            "decode_context_tokens": req.send_decode_id_offset - req.send_token_offset
                            if first_decode and req.send_token_offset > sent
                            else 0,
                            "history_tokens": len(req.output_ids)
                            if req.rid in finished_ids and req.send_token_offset > sent
                            else 0,
                        }
                        for req, (sent, first_decode) in zip(batch.reqs, output_before, strict=True)
                    ]
                }

        self._finished_ids.update(finished)
        return result

    def forward(self, original: Callable[..., Any], worker: Any, batch: Any, *args: Any, **kwargs: Any) -> Any:
        # Registered only for CPU sampling. Binding happens once per native
        # object, under observer exclusion; no layout or request metadata is read.
        with self._observer():
            self._regions.install(worker.model_runner.model, self._cpu)
            self._regions.install_components(worker.model_runner, self._cpu, "model_runner")
        return original(worker, batch, *args, **kwargs)

    def close(self) -> None:
        self._sampling.flush()
        if self._cpu is not None:
            self._cpu.flush()
        if not self._records:
            return

        self.directory.mkdir(parents=True, exist_ok=True)
        write_json(
            self.directory / f"rank-{self._rank}-environment.json",
            {
                "rank": self._rank,
                "pid": os.getpid(),
                "runtime": RuntimeEnvironment().describe(),
            },
        )
        with (self.directory / f"rank-{self._rank}-pid-{os.getpid()}.jsonl").open("a", encoding="utf-8") as stream:
            for record in self._records:
                stream.write(json.dumps(record, separators=(",", ":")) + "\n")
        self._records.clear()


class SGLangCpuWorkSampling(CpuFunction):
    def __init__(self, observer: SGLangHostSampling, binding: dict[str, Any]) -> None:
        super().__init__(observer._cpu, binding["cpu_name"])
        self.observer = observer
        self.binding = binding

    def _work(self, component: Any, arguments: inspect.BoundArguments) -> dict[str, Any] | None:
        if self.binding["semantic"] == "logical_collective":
            from sglang.srt.runtime_context import get_parallel

            tensor = arguments.arguments["input_"]
            return {
                "collective": {
                    "operation": self.binding["method"],
                    "ranks": list(range(get_parallel().tp_size)),
                    "input_shape": list(tensor.shape),
                    "data_type": str(tensor.dtype),
                    "host_message": None,
                }
            }

        request = NativeRegionWrapper.argument(arguments, self.binding["request_id_path"].rsplit(".", 1)[0])
        incoming, supported = self.observer._input_requests([request])
        if not supported or len(incoming) != 1:
            return None
        return {"incoming_requests": [[incoming[0]["prompt_tokens"], incoming[0]["output_tokens"]]]}

    def invoke(self, original: Callable[..., Any], component: Any, *args: Any, **kwargs: Any) -> Any:
        with self.observer._observer():
            active = bool(self.observer._scopes)
            if active:
                arguments = inspect.signature(original).bind(component, *args, **kwargs)
                contexts = self.observer._scopes[-1]["work_contexts"]
                contexts.append(self._work(component, arguments))
                context_id = len(contexts)
        if not active:
            return original(component, *args, **kwargs)

        # The native function record carries this work index and its real
        # clock boundaries. Metadata reads are excluded observer work.
        with self.sampling.function(self.name, context_id):
            return original(component, *args, **kwargs)


class SGLangCpuMessageSampling(CpuFunction):
    def __init__(self, observer: SGLangHostSampling, binding: dict[str, Any], channel: SGLangInputBroadcast) -> None:
        super().__init__(observer._cpu, binding["cpu_name"])
        self.observer = observer
        self.channel = channel
        self.semantic = binding["semantic"]

    def invoke(self, original: Callable[..., Any], *args: Any, **kwargs: Any) -> Any:
        with self.observer._observer():
            call = self.channel.describe(self.semantic, original, *args, **kwargs) if self.observer._scopes else None
            context_id = 0
            if call is not None and self.semantic == "input_message":
                contexts = self.observer._scopes[-1]["work_contexts"]
                contexts.append({"message": call.message})
                context_id = len(contexts)
        if call is None:
            return original(*args, **kwargs)

        # Semantic capture and CPU sampling enclose the same complete native
        # call. Children supply named clock boundaries, not overlapping work
        # contexts. PG1 timings never qualify logical-TP transport labels.
        with self.sampling.function(self.name, context_id):
            return self.channel.invoke_message(self.semantic, call, original, *args, **kwargs)


def install(prepared: dict[str, Any]) -> None:
    from sglang.srt.plugins.hook_registry import HookRegistry, HookType

    cpu = prepared["measurement_kind"] == "cpu"
    if cpu:
        from markov_collection.adapters.runtimes.pytorch.cpu_sampling import observe_operators

        observe_operators()

    observer = SGLangHostSampling(
        Path(os.environ["MARKOV_TRANSFER_SCOPES"]),
        set(prepared["measurement_ids"]) | set(prepared["warmup_ids"]) | set(prepared["control_ids"]),
        set(prepared["control_ids"]),
        prepared["native_regions"],
        cpu=cpu,
        source_rank=prepared.get("shard_execution", {}).get("rank"),
    )
    scheduler = "sglang.srt.managers.scheduler.Scheduler"
    batch = "sglang.srt.managers.schedule_batch.ScheduleBatch"
    HookRegistry.register(f"{batch}.prepare_for_extend", observer.prepare_prefill, HookType.AROUND)
    HookRegistry.register(f"{batch}.prepare_for_decode", observer.prepare_decode, HookType.AROUND)
    HookRegistry.register(f"{scheduler}.run_batch", observer.execute, HookType.AROUND)
    HookRegistry.register(f"{scheduler}.process_batch_result", observer.publish, HookType.AROUND)
    if cpu:
        HookRegistry.register(f"{batch}.filter_batch", observer.filter, HookType.AROUND)
        HookRegistry.register(f"{batch}.merge_batch", observer.merge, HookType.AROUND)
        HookRegistry.register(
            "sglang.srt.managers.tp_worker.TpModelWorker.forward_batch_generation", observer.forward, HookType.AROUND
        )

        from markov_collection.adapters.frameworks.sglang.inline import SGLangInlineMethod

        regions = prepared["native_regions"]
        input_broadcast = next(binding for binding in regions["bindings"] if binding["semantic"] == "input_broadcast")
        channel = SGLangInputBroadcast(input_broadcast["id"], observer._cpu)
        inline_methods = SGLangInlineMethod.group(regions)
        for hook, locations in inline_methods.items():
            scopes = {
                region.binding["id"]: CpuFunction(observer._cpu, region.binding["cpu_name"]) for region in locations
            }
            method = SGLangInlineMethod(locations, observer._cpu, scopes, record_work=False)
            HookRegistry.register(hook, method.invoke, HookType.AROUND)

        for binding in regions["bindings"]:
            hook = binding["hook"]
            if hook and hook not in inline_methods:
                if binding["semantic"] in {"input_message", "input_message_submit", "input_message_wait"}:
                    function = SGLangCpuMessageSampling(observer, binding, channel)
                elif binding["semantic"] in {"logical_collective", "input_request"}:
                    function = SGLangCpuWorkSampling(observer, binding)
                else:
                    function = CpuFunction(observer._cpu, binding["cpu_name"])
                HookRegistry.register(hook, function.invoke, HookType.AROUND)
        HookRegistry.register(input_broadcast["hook"], channel.invoke, HookType.AROUND)

    receive = next(
        binding["hook"] for binding in prepared["native_regions"]["bindings"] if binding["semantic"] == "input_receive"
    )
    HookRegistry.register(receive, observer.receive, HookType.AROUND)
    HookRegistry.register(f"{scheduler}.process_input_requests", observer.process, HookType.AROUND)
    HookRegistry.register(f"{scheduler}.get_next_batch_to_run", observer.schedule, HookType.AROUND)
    HookRegistry.register(f"{scheduler}.on_idle", observer.idle, HookType.AROUND)
