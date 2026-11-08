"""Bind persistent native request contents to the same fields in source and target graphs."""

from contextlib import contextmanager
from contextvars import ContextVar
from dataclasses import dataclass


@dataclass
class StateRecord:
    members: dict
    values: list | None


class RequestStateCalls:
    fields = ("output_token", "metadata")

    def __init__(self, observe=False) -> None:
        self.observe = observe
        self.batch = ContextVar("state_batch", default=None)
        self.receiving = ContextVar("metadata_receiving", default=None)
        self.accesses = ContextVar("request_state_accesses", default=None)

    @contextmanager
    def members(self, batch):
        previous = self.batch.set(batch)
        try:
            yield
        finally:
            self.batch.reset(previous)

    @contextmanager
    def record(self):
        access = StateRecord(
            {f"{field}/{mode}": [] for field in self.fields for mode in ("read", "write")},
            [] if self.observe else None,
        )
        previous = self.accesses.set(access)
        try:
            yield access
        finally:
            self.accesses.reset(previous)

    def note(self, field, direction, requests):
        access = self.accesses.get()
        if access is not None:
            key = f"{field}/{direction}"
            access.members[key] = list(dict.fromkeys([*access.members[key], *(req.rid for req in requests)]))

    @classmethod
    def condition(cls, accesses):
        return sorted(f"{field}/{mode}" for field in cls.fields for mode in ("read", "write") if accesses.get(f"{field}/{mode}"))

    @contextmanager
    def receive(self, decode_req):
        previous = self.receiving.set(decode_req)
        try:
            yield
        finally:
            self.receiving.reset(previous)

    def set_metadata(self, original, buffers, req):
        self.note("metadata", "write", [req])
        self.metadata_values("write", req, req.output_ids[0])
        return original(buffers, req)

    def metadata_values(self, operation, req, token):
        record = self.accesses.get()
        if record is None or record.values is None:
            return

        # These Python values enter set_buf or have already been read by native commit; never synchronize a device for diagnostics.
        record.values.append({
            "field": "metadata", "operation": operation, "request": req.rid,
            "room": req.bootstrap_room, "values": None if token is None else {
                "output_token": token,
                **{name: getattr(req, name) for name in ("cached_tokens", "cached_tokens_device", "cached_tokens_host", "cached_tokens_storage")},
            },
        })

    def get_metadata(self, original, buffers, index):
        if self.accesses.get() is not None:
            request = self.receiving.get()
            if request is None or request.metadata_buffer_index != index:
                raise ValueError("native metadata read has no matching request reservation")
            self.note("metadata", "read", [request.req])
        return original(buffers, index)

    def stash(self, original, relay, indices, payload):
        if self.accesses.get() is not None and indices.shape[0]:
            batch = self.batch.get()
            if batch is None or indices is not batch.req_pool_indices or not relay.spec_algo.is_none():
                raise ValueError("native relay write has no matching non-speculative request binding")
            self.note("output_token", "write", batch.reqs)
        return original(relay, indices, payload)

    def resolve(self, original, batch, relay):

        # These are the native read conditions before resolve_forward_inputs mutates the batch.
        if batch.prefill_input_ids_cpu is not None:
            if batch.mix_running_indices is not None:
                self.note("output_token", "read", batch.decoding_reqs)
        elif batch.input_ids is None and relay.spec_algo.is_none():
            self.note("output_token", "read", batch.reqs)
        return original(batch, relay)

    def install(self):
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        HookRegistry.register("sglang.srt.managers.overlap_utils.FutureMap.stash", self.stash, HookType.AROUND)
        HookRegistry.register("sglang.srt.managers.overlap_utils.resolve_forward_inputs", self.resolve, HookType.AROUND)
        HookRegistry.register("sglang.srt.disaggregation.utils.MetadataBuffers.set_buf", self.set_metadata, HookType.AROUND)
        HookRegistry.register("sglang.srt.disaggregation.utils.MetadataBuffers.get_buf", self.get_metadata, HookType.AROUND)
