"""Observe PD admission as per-request index copies and batch tensor construction."""

from __future__ import annotations

from contextlib import contextmanager, nullcontext
from typing import Any

import torch
from torch.overrides import TorchFunctionMode

from markov_collection.adapters.frameworks.sglang.batches import BatchTensors
from markov_collection.adapters.runtimes.pytorch.factories import TensorGeometryObservation
from markov_collection.adapters.runtimes.pytorch.indexing import TensorAssignmentCall, TensorViewCall
from markov_collection.instrumentation.calls import NativeCall
from markov_collection.instrumentation.loops import NativeLoop


class PrebuiltWrite(TensorAssignmentCall):
    def completed(self, args: tuple, kwargs: dict, result: Any, metadata: dict) -> str | None:
        error = super().completed(args, kwargs, result, metadata)
        metadata["argument_relations"] = [
            metadata["arguments"][0],
            {"slice": [{"scalar": "token_offset"}, {"scalar": ["add", "token_offset", "new_tokens"]}, None]},
            metadata["arguments"][-1],
        ]
        return error


class PrebuiltViews(TorchFunctionMode):
    def __init__(self, values: Any, mapping: Any, output: Any) -> None:
        super().__init__()
        self.values, self.mapping, self.output = values, mapping, output
        self.scope = values.recording.current_scope.get()
        self.row: Any = None

    def __torch_function__(self, function: Any, types: Any, args: tuple = (), kwargs: dict | None = None) -> Any:
        kwargs = kwargs or {}
        if self.values.recording.current_scope.get() != self.scope:
            return function(*args, **kwargs)
        if function is torch.Tensor.__setitem__ and args[0] is self.output:
            return PrebuiltWrite(
                self.values.recording,
                self.values,
                function.__qualname__,
                function,
                {},
                interface="tensor_assignment",
                mutations=(0,),
            )(*args, **kwargs)
        if function is not torch.Tensor.__getitem__ or (args[0] is not self.mapping and args[0] is not self.row):
            return function(*args, **kwargs)

        symbols = self.values.shapes
        whole_row = args[0] is self.mapping
        shape = (self.mapping.shape[1],) if whole_row else (symbols.symbol("new_tokens"),)
        start = (
            symbols.symbol("request_slot") * self.mapping.stride(0) if whole_row else symbols.symbol("prefix_tokens")
        )
        result = TensorViewCall(
            self.values.recording,
            self.values,
            function.__qualname__,
            function,
            {},
            interface="buffer_view",
            geometry=shape,
            start=start,
        )(*args, **kwargs)
        if whole_row:
            self.row = result
        return result


class PrebuiltColumns(TensorGeometryObservation):
    def __init__(self, values: Any, frame: Any) -> None:
        super().__init__(values, ("B",), (frame.f_locals["seq_lens"], frame.f_locals["req_pool_indices"]))

    def data_geometry(self, data: Any) -> tuple[str | int, ...] | None:
        if self.values.recording.current_scope.get() != self.scope:
            return None

        # Native row lists create B columns; the remaining constructor concatenates the input token arrays.
        return ("B",) if any(source is data for source in self.sources) else ("M",)


class PrebuiltLoop(NativeLoop):
    def __init__(self, values: Any, original: Any, batch: Any, prepare: bool) -> None:
        super().__init__(original)
        self.values, self.batch, self.prepare = values, batch, prepare

    @contextmanager
    def sequence(self, frame: Any) -> Any:
        inputs = {"request_map": self.batch.req_to_token_pool.req_to_token}
        if self.prepare:
            inputs["out_cache_loc"] = frame.f_locals["out_cache_loc"]
        with self.values.recording.scope(
            "prebuilt_members" if self.prepare else "prebuilt_cache",
            dynamic_calls=True,
            call_role="prebuilt_member" if self.prepare else "request_cache_retain",
            call_count="B",
            parameters={"B": len(self.batch.reqs)},
            ports={"inputs": self.values.port_values(inputs), "outputs": {}},
        ) as scope:
            yield
            if self.prepare:
                scope.metadata["ports"]["outputs"] = self.values.port_values(
                    {"out_cache_loc": frame.f_locals["out_cache_loc"]}
                )

    @contextmanager
    def part(self, kind: str, frame: Any) -> Any:
        if not self.prepare:
            with TensorGeometryObservation(self.values, ("B",)):
                yield
            return

        metadata: dict[str, Any] = {"entrypoint": "prebuilt_" + kind}
        inputs = {}
        if kind == "member":
            request = frame.f_locals["req"]
            inputs = {
                "request_map": self.batch.req_to_token_pool.req_to_token,
                "out_cache_loc": frame.f_locals["out_cache_loc"],
            }
            metadata["parameters"] = {
                "request_slot": request.req_pool_idx,
                "prefix_tokens": len(request.prefix_indices),
                "new_tokens": request.extend_range.length,
                "token_offset": frame.f_locals["offset"],
            }
        metadata["ports"] = {"inputs": self.values.port_values(inputs), "outputs": {}}
        with self.values.recording.scope("PrebuiltPart", **metadata) as scope:
            geometry = (
                PrebuiltColumns(self.values, frame)
                if kind == "suffix"
                else TensorGeometryObservation(self.values, factories={torch.empty: ("M",)})
            )
            views = (
                PrebuiltViews(self.values, inputs["request_map"], inputs["out_cache_loc"])
                if kind == "member"
                else nullcontext()
            )
            with geometry, views:
                yield
            outputs = (
                BatchTensors.read(self.batch)
                if kind == "suffix"
                else {"out_cache_loc": frame.f_locals["out_cache_loc"]}
            )
            scope.metadata["ports"]["outputs"] = self.values.port_values(outputs)


class PrebuiltCall(NativeCall):
    def __init__(self, values: Any, original: Any, batch: Any, prepare: bool, relay: Any = None) -> None:
        super().__init__(values.recording, values, original.__qualname__, original, {})
        self.batch, self.prepare = batch, prepare
        self.relay = relay

    def resources(self, arguments: dict) -> tuple[Any, Any]:
        inputs = {"request_map": self.batch.req_to_token_pool.req_to_token}
        if not self.prepare:
            inputs["request_slots"] = self.batch.req_pool_indices
            inputs["token_relay"] = self.relay.output_tokens_buf
        self.input_ports = self.values.port_values(inputs)
        return tuple(inputs.values()), ()

    def invoke(self, *args: Any, **kwargs: Any) -> Any:
        with PrebuiltLoop(self.values, self.original, self.batch, self.prepare):
            return super().invoke(*args, **kwargs)

    def outputs(self, result: Any) -> Any:
        return self.output_tensors

    def completed(self, args: tuple, kwargs: dict, result: Any, metadata: dict) -> None:
        self.output_tensors = (
            BatchTensors.read(self.batch) if self.prepare else {"token_relay": args[2].output_tokens_buf}
        )
        metadata.update(
            entrypoint="prebuilt_prepare" if self.prepare else "prebuilt_process",
            entrypoint_phase=None,
            parameters={
                "M": sum(request.extend_range.length for request in self.batch.reqs),
                "B": len(self.batch.reqs),
            },
            ports={"inputs": self.input_ports, "outputs": self.values.port_values(self.output_tensors)},
        )
