"""Expose the native allocator's prefix and ordered cold-Prefill members."""

from __future__ import annotations

import inspect
from contextlib import contextmanager, nullcontext
from typing import Any

import torch
from torch.overrides import TorchFunctionMode

from markov_collection.adapters.runtimes.pytorch.indexing import TensorViewCall
from markov_collection.instrumentation.loops import NativeLoop


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


class AllocationLoop(NativeLoop):
    def __init__(
        self, values: Any, original: Any, lengths: tuple[int, ...], contexts: tuple[int, ...], page_size: int
    ) -> None:
        super().__init__(original)
        self.values = values
        self.recording = values.recording
        self.lengths = lengths
        self.contexts = contexts
        self.page_size = page_size

    @contextmanager
    def sequence(self, frame: Any) -> Any:
        self.buffers = {name: value for name, value in frame.f_locals.items() if isinstance(value, torch.Tensor)}
        with self.recording.scope(
            "allocation_members",
            dynamic_calls=True,
            call_role="allocation_member",
            call_count="B",
            parameters={"B": len(self.lengths)},
            ports={"inputs": self.values.port_values(self.buffers), "outputs": {}},
        ) as scope:
            yield
            scope.metadata["ports"]["outputs"] = self.values.port_values({"out_indices": frame.f_locals["out_indices"]})

    @contextmanager
    def part(self, kind: str, frame: Any) -> Any:
        if kind == "suffix":
            yield
            return

        metadata = {"entrypoint": "allocation_" + kind}
        if kind == "prefix":
            inputs = {name: value for name, value in frame.f_locals.items() if isinstance(value, torch.Tensor)}
        else:
            inputs = self.buffers
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
            metadata.update(
                parameters=parameters,
                bindings={"page_size": self.page_size},
                derived_parameters=[
                    {"name": "full_pages", "expression": ["floor_div", "new_tokens", "page_size"]},
                    {"name": "tail_tokens", "expression": ["mod", "new_tokens", "page_size"]},
                ],
                guards=[{"condition": ["eq", "context_tokens", 0], "expected": True}]
                + [
                    {"condition": ["eq", name, 0], "expected": parameters[name] == 0}
                    for name in ("full_pages", "tail_tokens")
                ],
            )
        metadata["ports"] = {"inputs": self.values.port_values(inputs), "outputs": {}}
        with self.recording.scope("AllocationPart", **metadata) as scope:
            views = AllocationViews(self.values, self.buffers) if kind == "member" else nullcontext()
            with views:
                yield
            with self.values.metadata():
                produced = (
                    {"out_indices": frame.f_locals["out_indices"]}
                    if kind == "member"
                    else {
                        name: value
                        for name, value in frame.f_locals.items()
                        if isinstance(value, torch.Tensor) and name not in inputs
                    }
                )
                scope.metadata["ports"]["outputs"] = self.values.port_values(produced)


class SGLangAllocation:
    def __init__(self, operators: Any, page_size: int, device: str, need_sort: bool) -> None:
        from sglang.srt.hardware_backend.npu.allocator_npu import NPUPagedTokenToKVPoolAllocator

        self.operators = operators
        self.page_size = page_size
        self.device = device
        self.need_sort = need_sort
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
        from markov_collection.adapters.frameworks.sglang.state import PageAllocationCall

        values = self.operators.values
        with values.auxiliary():
            pages = sum((tokens - 1) // self.page_size + 1 for tokens in lengths)
            pool = self.pool_type(
                pages * self.page_size, self.page_size, torch.bfloat16, self.device, None, self.need_sort
            )

            # SGLang alloc_for_extend supplies int64 length vectors; no model or request state is recreated.
            prefix_cpu = torch.zeros(len(lengths), dtype=torch.int64)
            lengths_cpu = torch.tensor(lengths, dtype=torch.int64)
            prefix, sequences = prefix_cpu.to(self.device), lengths_cpu.to(self.device)
            last = torch.full((len(lengths),), -1, dtype=torch.int64, device=self.device)
        arguments = (pool, prefix, prefix_cpu, sequences, lengths_cpu, last, sum(lengths))
        if observed is None:
            return self.original(*arguments)

        with self.operators.prototype(
            "AllocationPrototype", observed, phase="prefill", M=sum(lengths), B=len(lengths)
        ):
            if observed:
                return PageAllocationCall(values, self.original, pool, decode=False)(*arguments)
            return self.original(*arguments)

    def warmup(self) -> None:
        if self.warmed:
            return
        for point in self.points():
            self.execute(point, None)
        if self.need_sort:
            self.merge(None)
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
        if self.need_sort:
            baseline = self.merge(False)
            observed = self.merge(True)
            with self.operators.values.auxiliary():
                if not all(torch.equal(left, right) for left, right in zip(baseline, observed, strict=True)):
                    raise RuntimeError("allocator merge observation changed native output")
        self.captured = True

    def merge(self, observed: bool | None) -> tuple[torch.Tensor, torch.Tensor]:
        from markov_collection.adapters.frameworks.sglang.state import PageMergeCall

        values = self.operators.values
        with values.auxiliary():
            pool = self.pool_type(4 * self.page_size, self.page_size, torch.bfloat16, self.device, None, True)
            allocated = pool.alloc(2 * self.page_size)
            pool.free(allocated)
        original = inspect.unwrap(type(pool).merge_and_sort_free)
        if observed is None:
            original(pool)
            return pool.free_pages, pool.release_pages

        with self.operators.prototype("AllocationPrototype", observed, phase="control"):
            if observed:
                PageMergeCall(values, original, pool)(pool)
            else:
                original(pool)
        return pool.free_pages, pool.release_pages

    @staticmethod
    def declare(regions: list[dict], operations: list[dict], values: list[dict]) -> None:
        from markov_collection.adapters.runtimes.pytorch.indexing import IndexedRegions

        indexed = IndexedRegions(regions, operations, values)
        for member in regions:
            metadata = member["metadata"]
            if metadata.get("entrypoint") == "kv_allocate" and "release_pages" in metadata["ports"]["outputs"]:

                # The merge clears this named pool; it is not the allocator's M-element output buffer.
                empty = values[metadata["ports"]["outputs"]["release_pages"]]
                producer = operations[empty["producer"]]
                if producer["name"] != "aten.empty.memory_format" or producer["arguments"][0] != [0]:
                    raise ValueError("native allocation must clear its deferred pool with an empty tensor")
                empty["shape_relation"] = [0]
                producer["argument_relations"] = [{"shape": [0]}]

            if metadata.get("entrypoint") != "allocation_member":
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
