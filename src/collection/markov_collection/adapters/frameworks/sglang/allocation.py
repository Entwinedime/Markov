"""Expose the native allocator's prefix and ordered cold-Prefill members."""

from __future__ import annotations

import ast
import inspect
import sys
from typing import Any

import torch
from torch.overrides import TorchFunctionMode

from markov_collection.adapters.runtimes.pytorch.indexing import TensorViewCall


class AllocationViews(TorchFunctionMode):
    def __init__(self, values: Any, buffers: dict[str, torch.Tensor]) -> None:
        super().__init__()
        self.values = values
        self.buffers = {id(value): name for name, value in buffers.items()}
        self.scope = values.recording.current_scope.get()

    def __torch_function__(self, function: Any, types: Any, args: tuple = (), kwargs: dict | None = None) -> Any:
        kwargs = kwargs or {}
        if function is not torch.Tensor.__getitem__ or self.values.recording.current_scope.get() != self.scope:
            return function(*args, **kwargs)
        name = self.buffers.get(id(args[0]))
        if name is None:
            return function(*args, **kwargs)

        symbols = self.values.shapes

        # Cold-prefix page offsets come from the native allocator's equations, not fitted observed integers.
        if isinstance(args[1], int):
            geometry, start = (), symbols.symbol("member_index")
        elif name == "free_pages":
            geometry = (symbols.symbol("full_pages"),) if isinstance(args[1], slice) else ()
            start = symbols.symbol("page_offset") + (0 if geometry else symbols.symbol("full_pages"))
        elif name == "pos_in_page":
            geometry, start = (symbols.symbol("tail_tokens"),), 0
        else:
            return function(*args, **kwargs)

        return TensorViewCall(
            self.values.recording,
            self.values,
            "torch.Tensor.__getitem__",
            function,
            {},
            interface="buffer_view",
            geometry=geometry,
            start=start,
        )(*args, **kwargs)


class AllocationLoop:
    def __init__(
        self, values: Any, original: Any, lengths: tuple[int, ...], contexts: tuple[int, ...], page_size: int
    ) -> None:
        self.values = values
        self.recording = values.recording
        self.lengths = lengths
        self.contexts = contexts
        self.page_size = page_size
        self.code = original.__code__
        source, start = inspect.getsourcelines(original)
        loop = next(node for node in ast.walk(ast.parse("".join(source))) if isinstance(node, ast.For))
        self.header = start + loop.lineno - 1
        self.end = start + loop.end_lineno - 1
        self.part: Any = None
        self.members: Any = None
        self.error: tuple = (None, None, None)
        self.frame: Any = None
        self.views: Any = None
        self.inputs: dict[str, torch.Tensor] = {}
        self.buffers: dict[str, torch.Tensor] = {}

    def __enter__(self) -> AllocationLoop:

        # Trace only this native helper; its Python statements and device calls remain unchanged.
        self.previous = sys.gettrace()
        sys.settrace(self.trace)
        return self

    def close_part(self, error: tuple = (None, None, None)) -> None:
        if self.part is not None:
            if self.views is not None:
                self.views.__exit__(*error)
                self.views = None
            with self.values.metadata():
                produced = {
                    name: value
                    for name, value in self.frame.f_locals.items()
                    if isinstance(value, torch.Tensor) and name not in self.inputs
                }
                if self.part.metadata["entrypoint"] == "allocation_member":
                    produced = {"out_indices": self.frame.f_locals["out_indices"]}
                self.part.metadata["ports"]["outputs"] = self.values.port_values(produced)
            self.part.__exit__(*error)
            self.part = None

    def __exit__(self, *error: Any) -> None:
        sys.settrace(self.previous)
        self.close_part(error)
        if self.members is not None:
            self.members.__exit__(*error)
        self.buffers.clear()
        self.inputs.clear()
        self.frame = None

    def trace(self, frame: Any, event: str, result: Any) -> Any:
        if frame.f_code is not self.code:
            return None
        if event == "call":
            self.frame = frame
            self.inputs = {name: value for name, value in frame.f_locals.items() if isinstance(value, torch.Tensor)}
            self.part = self.recording.scope(
                "AllocationPart",
                entrypoint="allocation_prefix",
                ports={"inputs": self.values.port_values(self.inputs), "outputs": {}},
            )
            self.part.__enter__()
        elif event == "line" and frame.f_lineno == self.header:
            self.close_part()
            if self.members is None:
                self.buffers = {
                    name: value for name, value in frame.f_locals.items() if isinstance(value, torch.Tensor)
                }
                self.members = self.recording.scope(
                    "allocation_members",
                    dynamic_calls=True,
                    call_role="allocation_member",
                    call_count="B",
                    parameters={"B": len(self.lengths)},
                    ports={"inputs": self.values.port_values(self.buffers), "outputs": {}},
                )
                self.members.__enter__()
        elif event == "line" and self.part is None and self.header < frame.f_lineno <= self.end:
            index = frame.f_locals["i"]
            tokens = self.lengths[index]
            parameters = {
                "member_index": index,
                "new_tokens": tokens,
                "context_tokens": self.contexts[index],
                "full_pages": tokens // self.page_size,
                "tail_tokens": tokens % self.page_size,
                "page_offset": sum((length - 1) // self.page_size + 1 for length in self.lengths[:index]),
                "token_offset": sum(self.lengths[:index]),
            }
            guards = [{"condition": ["eq", "context_tokens", 0], "expected": True}] + [
                {"condition": ["eq", name, 0], "expected": parameters[name] == 0}
                for name in ("full_pages", "tail_tokens")
            ]
            self.part = self.recording.scope(
                "AllocationPart",
                entrypoint="allocation_member",
                parameters=parameters,
                bindings={"page_size": self.page_size},
                derived_parameters=[
                    {"name": "full_pages", "expression": ["floor_div", "new_tokens", "page_size"]},
                    {"name": "tail_tokens", "expression": ["mod", "new_tokens", "page_size"]},
                ],
                guards=guards,
                ports={"inputs": self.values.port_values(self.buffers), "outputs": {}},
            )
            self.part.__enter__()
            self.views = AllocationViews(self.values, self.buffers)
            self.views.__enter__()
        elif event == "return":
            self.close_part(self.error)
            if self.members is not None:
                self.members.metadata["ports"]["outputs"] = self.values.port_values(
                    {"out_indices": frame.f_locals["out_indices"]}
                )
                self.members.__exit__(*self.error)
                self.members = None
        elif event == "exception":
            self.error = result

        return self.trace


class SGLangAllocation:
    def __init__(self, operators: Any, page_size: int, device: str) -> None:
        from sglang.srt.hardware_backend.npu.allocator_npu import NPUPagedTokenToKVPoolAllocator

        self.operators = operators
        self.recording = operators.recording
        self.page_size = page_size
        self.device = device
        self.pool_type = NPUPagedTokenToKVPoolAllocator
        self.original = inspect.unwrap(self.pool_type.alloc_extend)
        self.warmed = False
        self.captured = False

    def points(self) -> tuple[tuple[int, ...], ...]:
        from markov_collection.adapters.frameworks.sglang.state import PageAllocationCall

        page = self.page_size
        boundary = PageAllocationCall.parallel_page_limit
        return (
            (page - 1, boundary * page, 2 * page + 1),
            (page + 1, boundary // 2 * page, page - 1, boundary // 2 * page, 2 * page),
        )

    def execute(self, lengths: tuple[int, ...], observed: bool | None) -> torch.Tensor:
        from contextlib import nullcontext

        from markov_collection.adapters.frameworks.sglang.state import PageAllocationCall
        from markov_collection.adapters.runtimes.pytorch.observation import TorchMarkers

        values = self.operators.values
        with values.auxiliary():
            pages = sum((tokens - 1) // self.page_size + 1 for tokens in lengths)
            pool = self.pool_type(pages * self.page_size, self.page_size, torch.bfloat16, self.device, None, False)

            # SGLang alloc_for_extend supplies int64 length vectors; no model or request state is recreated.
            prefix_cpu = torch.zeros(len(lengths), dtype=torch.int64)
            lengths_cpu = torch.tensor(lengths, dtype=torch.int64)
            prefix, sequences = prefix_cpu.to(self.device), lengths_cpu.to(self.device)
            last = torch.full((len(lengths),), -1, dtype=torch.int64, device=self.device)
        arguments = (pool, prefix, prefix_cpu, sequences, lengths_cpu, last, sum(lengths))
        if observed is None:
            return self.original(*arguments)

        identity = len(self.recording.batches)
        self.recording.batches.append(
            {
                "id": identity,
                "kind": "primitive",
                "phase": "prefill",
                "members": [],
                "parameters": {"M": sum(lengths), "B": len(lengths)},
                "observation": "source" if observed else "baseline",
            }
        )
        if observed:
            self.operators.begin_batch()
        scope = (
            self.recording.scope("AllocationPrototype", batch=identity)
            if observed
            else TorchMarkers.scope(f"markov.baseline/{identity}")
        )
        with scope, self.operators if observed else nullcontext():
            if observed:
                return PageAllocationCall(values, self.original, pool, decode=False)(*arguments)
            return self.original(*arguments)

    def warmup(self) -> None:
        if self.warmed:
            return
        for point in self.points():
            self.execute(point, None)
        if torch.device(self.device).type == "npu":
            torch.npu.synchronize()
        self.warmed = True

    def capture(self) -> None:
        if self.captured:
            return
        for point in self.points():
            baseline = self.execute(point, False)
            observed = self.execute(point, True)
            with self.operators.values.auxiliary():
                if not torch.equal(baseline, observed):
                    raise RuntimeError("allocator observation changed native output")
        self.captured = True

    @staticmethod
    def declare(regions: list[dict], operations: list[dict], values: list[dict]) -> None:
        from markov_collection.adapters.runtimes.pytorch.indexing import IndexedRegions

        indexed = IndexedRegions(regions, operations, values)
        for member in regions:
            if member["metadata"].get("entrypoint") != "allocation_member":
                continue
            written: Any = 0
            for identity in indexed.children[member["id"]]:
                metadata = regions[identity]["metadata"]
                if metadata.get("interface") == "buffer_view":
                    arguments = metadata["argument_relations"]
                    indexed.declare(identity, arguments[1]["shape"], arguments[2]["scalar"])
                elif metadata.get("interface") == "tensor_assignment":
                    source = values[metadata["arguments"][-1]["value"]]
                    indexed.declare(identity, source["shape_relation"], ["add", "token_offset", written])
                    written = ["add", written, source["shape_relation"][0]]
