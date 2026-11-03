"""Expose native page allocation through its existing tail, full pages and new tail."""

from __future__ import annotations

import inspect
from contextlib import contextmanager, nullcontext
from itertools import accumulate, product
from typing import Any

import torch
from torch.overrides import TorchFunctionMode

from markov_collection.adapters.runtimes.pytorch.indexing import TensorViewCall
from markov_collection.instrumentation.loops import NativeLoop


class AllocationViews(TorchFunctionMode):
    def __init__(self, values: Any, buffers: dict[str, torch.Tensor], frame: Any) -> None:
        super().__init__()
        self.values = values
        self.buffers = {id(value): name for name, value in buffers.items()}
        self.frame = frame
        self.scope = values.recording.current_scope.get()

    def __torch_function__(self, function: Any, types: Any, args: tuple = (), kwargs: dict | None = None) -> Any:
        kwargs = kwargs or {}
        if function is not torch.Tensor.__getitem__ or self.values.recording.current_scope.get() != self.scope:
            return function(*args, **kwargs)
        name = self.buffers.get(id(args[0]))
        if name is None:
            return function(*args, **kwargs)

        symbols = self.values.shapes

        # Offsets follow the native loop; equal observed scalar values never identify a slice's meaning.
        if isinstance(args[1], int):
            geometry, start = (), symbols.symbol("member_index")
        elif name == "free_pages":
            geometry = (symbols.symbol("full_pages"),) if isinstance(args[1], slice) else ()
            start = symbols.symbol("page_offset") + (0 if geometry else symbols.symbol("full_pages"))
        elif name == "pos_in_page":
            part = "head_tokens" if args[1].stop is self.frame.f_locals["num1"] else "tail_tokens"
            geometry, start = (symbols.symbol(part),), 0
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
        pages = (
            (context + length + page_size - 1) // page_size - (context + page_size - 1) // page_size
            for length, context in zip(lengths, contexts, strict=True)
        )
        self.page_offsets = tuple(accumulate(pages, initial=0))
        self.token_offsets = tuple(accumulate(lengths, initial=0))

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
            context = self.contexts[index]
            head = min(tokens, -context % self.page_size)
            remaining = tokens - head
            parameters = {
                "member_index": index,
                "new_tokens": tokens,
                "context_tokens": context,
                "head_tokens": head,
                "remaining_tokens": remaining,
                "full_pages": remaining // self.page_size,
                "tail_tokens": remaining % self.page_size,
                "page_offset": self.page_offsets[index],
                "token_offset": self.token_offsets[index],
            }
            metadata.update(
                parameters=parameters,
                bindings={"page_size": self.page_size},
                derived_parameters=[
                    {
                        "name": "head_tokens",
                        "expression": ["min", "new_tokens", ["mod", ["mul", -1, "context_tokens"], "page_size"]],
                    },
                    {"name": "remaining_tokens", "expression": ["add", "new_tokens", ["mul", -1, "head_tokens"]]},
                    {"name": "full_pages", "expression": ["floor_div", "remaining_tokens", "page_size"]},
                    {"name": "tail_tokens", "expression": ["mod", "remaining_tokens", "page_size"]},
                ],
                guards=[
                    {"condition": ["eq", name, 0], "expected": parameters[name] == 0}
                    for name in ("head_tokens", "full_pages", "tail_tokens")
                ],
            )
        metadata["ports"] = {"inputs": self.values.port_values(inputs), "outputs": {}}
        with self.recording.scope("AllocationPart", **metadata) as scope:
            views = AllocationViews(self.values, self.buffers, frame) if kind == "member" else nullcontext()
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

    def points(self) -> list[tuple[tuple[int, ...], tuple[int, ...], int | None]]:
        from markov_collection.adapters.frameworks.sglang.state import PageAllocationCall

        page = self.page_size
        boundary = PageAllocationCall.parallel_page_limit
        ready = (None, 0, 1) if self.need_sort else (None,)
        points = [((boundary * page,), (0,), count) for count in ready]
        if torch.device(self.device).type == "npu":
            points[:0] = [((page + 1,), (0,), count) for count in ready]

        # Each segment is absent, singleton or non-singleton; one batch covers this finite native basis.
        fragments = sorted({extent for extent in (0, 1, page - 1) if extent < page})
        members = [
            (head + full * page + tail, page - head if head else 0)
            for head, full, tail in product(fragments, (0, 1, 2), fragments)
            if head or full or tail
        ]
        lengths, contexts = zip(*members, strict=True)

        # The final member selects the native large-allocation implementation independently of the basis size.
        points.append(((*lengths, boundary * page), (*contexts, 0), None))
        return points

    def execute(
        self, lengths: tuple[int, ...], contexts: tuple[int, ...], ready_pages: int | None, observed: bool | None
    ) -> torch.Tensor:
        from markov_collection.adapters.frameworks.sglang.state import PageAllocationCall

        values = self.operators.values
        with values.auxiliary():
            sequences = tuple(context + length for context, length in zip(contexts, lengths, strict=True))
            prefix_pages = tuple((context + self.page_size - 1) // self.page_size for context in contexts)
            total_pages = sum((tokens + self.page_size - 1) // self.page_size for tokens in sequences)
            pages = total_pages - sum(prefix_pages)
            pool = self.pool_type(
                total_pages * self.page_size, self.page_size, torch.bfloat16, self.device, None, self.need_sort
            )
            last = torch.full((len(lengths),), -1, dtype=torch.int64, device=self.device)
            if any(prefix_pages):

                # Allocate real prefix pages so the native tail path reads a valid last location.
                prefix_indices = pool.alloc(sum(prefix_pages) * self.page_size)
                offset = 0
                for index, (context, count) in enumerate(zip(contexts, prefix_pages, strict=True)):
                    if context:
                        last[index] = prefix_indices[offset * self.page_size + context - 1]
                    offset += count

            # Exercise merging through the actual allocator, preserving its page-count read before the merge.
            if ready_pages is not None:
                allocated = pool.alloc((pages - ready_pages) * self.page_size)
                pool.free(allocated)

            # SGLang alloc_for_extend supplies int64 length vectors; no model or request state is recreated.
            prefix_cpu = torch.tensor(contexts, dtype=torch.int64)
            sequences_cpu = torch.tensor(sequences, dtype=torch.int64)
            prefix, sequences_device = prefix_cpu.to(self.device), sequences_cpu.to(self.device)
        arguments = (pool, prefix, prefix_cpu, sequences_device, sequences_cpu, last, sum(lengths))
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
        for lengths, contexts, ready in self.points():
            self.execute(lengths, contexts, ready, None)
        if torch.device(self.device).type == "npu":
            torch.npu.synchronize()
        self.warmed = True

    def capture(self) -> None:
        if self.captured:
            return
        for lengths, contexts, ready in self.points():
            baseline = self.execute(lengths, contexts, ready, False)
            observed = self.execute(lengths, contexts, ready, True)
            with self.operators.values.auxiliary():
                if not torch.equal(baseline, observed):
                    raise RuntimeError("allocator observation changed native output")
        self.captured = True

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

            # A member consumes the current pool, independently of whether its caller first merged deferred pages.
            pool = metadata["ports"]["inputs"]["free_pages"]
            metadata["parameters"]["allocation_pool_pages"] = values[pool]["shape"][0]
            contracts = {item["value"]: item["shape"] for item in metadata.get("shape_contracts", [])}
            contracts[pool] = ["allocation_pool_pages"]
            metadata["shape_contracts"] = [{"value": value, "shape": shape} for value, shape in contracts.items()]

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
