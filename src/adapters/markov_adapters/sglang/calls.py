"""Interleave native calls with their caller's execution segments."""

from __future__ import annotations

from contextlib import contextmanager, nullcontext
from contextvars import ContextVar
from dataclasses import dataclass, field
from typing import Any

from markov_adapters.sglang.pipeline import PipelineABI


@dataclass
class ActionCall:
    operation: str
    parameters: dict[str, int]
    work: dict[str, Any]
    inputs: dict[str, Any] = field(default_factory=dict)
    outputs: dict[str, Any] = field(default_factory=dict)
    condition: dict[str, Any] = field(default_factory=dict)
    guards: tuple = ()


@dataclass
class CallFrame:
    operation: str
    original: Any
    parameters: dict[str, int]
    work: dict[str, Any]
    inputs: Any = None
    guards: tuple = ()
    host_only: bool = False
    outputs: list[dict[str, Any]] = field(default_factory=list)
    entry: str = "call.enter"
    exit: str = ""
    entry_context: dict = field(default_factory=dict)
    exit_context: dict = field(default_factory=dict)
    segment: Any = None
    values: dict = field(default_factory=dict)
    observations: list[int] = field(default_factory=list)
    tensor_inputs: dict = field(default_factory=dict)
    tensor_outputs: dict = field(default_factory=dict)

    @property
    def first(self):
        return self.entry == "call.enter"

    @property
    def terminal(self):
        return self.exit == "call.return"

    @property
    def continuation(self):
        return {
            "owner": self.operation, "entry": self.entry, "exit": self.exit,
            "entry_context": self.entry_context, "exit_context": self.exit_context,
        }

    def child(self, work):

        # Preserve the caller's logical position, not a run-local call counter or resource binding.
        work["callers"] = [*self.work.get("callers", ()), {
            "owner": self.operation, "context": self.work.get("context", {}),
            "entry": self.entry, "entry_context": self.entry_context,
        }]


class CallSegments:
    def __init__(self) -> None:
        self.current = ContextVar("native_call_segments", default=None)
        self.serving = False

    def input(self, original, scheduler, requests):
        from sglang.srt.managers.io_struct import BatchTokenizedGenerateReqInput, TokenizedGenerateReqInput

        # Profiling and cache-reset commands must not restart a drained serving interval.
        if any(isinstance(request, (TokenizedGenerateReqInput, BatchTokenizedGenerateReqInput)) for request in requests):
            self.serving = True
        return original(scheduler, requests)

    def idle(self, original, scheduler):

        # A native idle boundary ends logical work, not the lifetime of submitted transfers or profiling.
        if scheduler.is_fully_idle():
            self.serving = False
        return original(scheduler)

    def install_lifecycle(self):
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        for name, hook in (("process_input_requests", self.input), ("on_idle", self.idle)):
            HookRegistry.register(f"sglang.srt.managers.scheduler.Scheduler.{name}", hook, HookType.AROUND)

    def enclosing(self, frame):
        return nullcontext()

    def fragment(self, frame):
        raise NotImplementedError

    def request_id(self, native_id):
        raise NotImplementedError

    def read_boundary(self, kind, original, work):
        raise NotImplementedError

    @contextmanager
    def read(self, kind, original, work):
        work = dict(work)
        work["context"] = {**work["context"], "query": kind}

        # Finish preceding CPU work before observing state; do not add a wait for the queried producer.
        with self.separate("query", work), self.read_boundary(kind, original, work) as query:
            yield query

    def progress(self, requests):

        # Logical request progress is comparable across runs; native slots and object handles are not.
        return [
            {
                "id": self.request_id(req.rid), "generated_tokens": len(req.output_ids),
                "committed_tokens": req.kv_committed_len,
                "extend": None if req.extend_range is None else [req.extend_range.start, req.extend_range.end],
            }
            for req in requests
        ]

    def request_work(self, request, begin=0, end=None):
        end = len(request.origin_input_ids) if end is None else end
        work = self.work(None, [{"id": request.rid, "context_tokens": begin, "new_tokens": end - begin}])

        # Token coverage describes the graph's accesses; request progress identifies this native invocation.
        work["context"] = {"request": self.progress((request,))[0]}
        return work

    def scheduler_work(self):
        scheduler = self.scheduler
        queues = {
            "waiting": scheduler.waiting_queue,
            "running": scheduler.running_batch.reqs,
            "previous": scheduler.last_batch.reqs if scheduler.last_batch is not None else (),
            "chunked": (scheduler.chunked_req,) if scheduler.chunked_req is not None else (),
        }
        mode = scheduler.server_args.disaggregation_mode
        if mode == "prefill":
            queues.update(
                bootstrap=scheduler.disagg_prefill_bootstrap_queue.queue,
                transferring=scheduler.disagg_prefill_inflight_queue,
            )
        elif mode == "decode":
            prealloc = scheduler.disagg_decode_prealloc_queue
            queues.update(
                prealloc=[entry.req for entry in prealloc.queue],
                pending=[entry.req for entry in prealloc.pending_reqs],
                retracted=prealloc.retracted_queue,
                transferring=[entry.req for entry in scheduler.disagg_decode_transfer_queue.queue],
            )

        # Snapshot native membership and progress, without inventing a unique identity for repeated polls.
        work = self.work(None)
        work["context"] = {name: self.progress(requests) for name, requests in queues.items()}
        if scheduler.ps.pp_size > 1:
            work["context"]["pipeline"] = PipelineABI.control_context(self)
        return work

    def resume(self, frame, entry="call.enter", context=None):
        frame.entry = entry
        frame.entry_context = {} if context is None else context
        frame.exit = ""
        frame.exit_context = {}
        frame.segment = self.fragment(frame)
        frame.segment.__enter__()

    def suspend(self, frame, boundary, context=None, error=None):
        segment, frame.segment = frame.segment, None
        if segment is None:
            return
        frame.exit = boundary
        frame.exit_context = {} if context is None else context
        segment.__exit__(type(error) if error is not None else None, error, error.__traceback__ if error is not None else None)

    @contextmanager
    def separate(self, operation, work):
        parent = self.current.get()
        context = work.get("context", {})
        if parent is not None:
            parent.child(work)
            self.suspend(parent, operation, context)
        previous = self.current.set(None)
        try:
            yield
        finally:
            self.current.reset(previous)

        # Independently submitted actions must not also appear inside their caller's segment.
        if parent is not None:
            self.resume(parent, operation, context)

    @contextmanager
    def call(self, operation, original, *, parameters=None, work=None, inputs=None, guards=(), host_only=False, tensor_inputs=None):
        parent = self.current.get()
        work = dict(parent.work if work is None else work)
        context = work.get("context", {})
        if parent is not None:
            parent.child(work)
            self.suspend(parent, operation, context)
        frame = CallFrame(
            operation, original,
            dict(parent.parameters if parameters is None else parameters),
            work, inputs, guards, host_only,
            tensor_inputs={} if tensor_inputs is None else tensor_inputs,
        )
        previous = self.current.set(frame)
        try:
            with self.enclosing(frame):
                self.resume(frame)
                try:
                    yield frame
                except BaseException as error:
                    self.suspend(frame, "call.error", error=error)
                    raise
                else:
                    self.suspend(frame, "call.return")
        finally:
            self.current.reset(previous)

        # Resume only after a successful child return; a failing call cannot publish another successful segment.
        if parent is not None:
            self.resume(parent, operation, context)
