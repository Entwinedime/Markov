"""Expose native KV transitions, visible transfers and named tensor ports."""

from __future__ import annotations

from contextlib import nullcontext
from typing import Any

from markov_collection.adapters.runtimes.pytorch.concatenation import ConcatenationObservation
from markov_collection.adapters.runtimes.pytorch.factories import TensorGeometryObservation
from markov_collection.adapters.runtimes.pytorch.indexing import TensorViewObservation
from markov_collection.adapters.runtimes.pytorch.unique import UniqueObservation
from markov_collection.instrumentation.calls import NativeCall


class RequestCacheCall(NativeCall):
    def __init__(self, values: Any, original: Any, cache: Any, request: Any, finished: bool) -> None:
        super().__init__(values.recording, values, original.__qualname__, original, {})
        self.cache = cache
        self.request = request
        self.finished = finished
        self.immediate = finished and cache.token_to_kv_pool_allocator.is_not_in_free_group
        self.pool_name = "release_pages" if cache.token_to_kv_pool_allocator.need_sort else "free_pages"

    def resources(self, arguments: dict[str, Any]) -> tuple[Any, Any]:
        self.input_ports = self.values.port_values({"request_map": self.cache.req_to_token_pool.req_to_token})
        if self.immediate:
            self.input_ports.update(
                self.values.port_values(
                    {self.pool_name: getattr(self.cache.token_to_kv_pool_allocator, self.pool_name)}
                )
            )

        return (self.cache.req_to_token_pool.req_to_token,), ()

    def invoke(self, *args: Any, **kwargs: Any) -> Any:
        with self.values.metadata():
            mapping = self.cache.req_to_token_pool.req_to_token
            slot = self.request.req_pool_idx

            # Read the same native length without consuming the request's release state.
            length = self.request._cache_commit_len() if self.finished else self.request.fill_len
            metadata = self.recording.regions[self.recording.current_scope.get()]["metadata"]
            metadata["entrypoint"] = "request_cache_release" if self.finished else "request_cache_retain"
            metadata["entrypoint_phase"] = None
            metadata["parameters"] = {"request_slot": slot, "resident_tokens": length}
            metadata["bindings"] = {"request_map_stride": mapping.stride(0)}
            metadata["guards"] = [
                {"condition": ["lt", 0, "request_slot"], "expected": True},
                {"condition": ["le", "request_slot", "R"], "expected": True},
                {"condition": ["le", 0, "resident_tokens"], "expected": True},
                {"condition": ["le", "resident_tokens", "request_map_stride"], "expected": True},
            ]
            if self.finished:
                metadata["parameters"]["released_tokens"] = length
                metadata["derived_parameters"] = [{"name": "released_tokens", "expression": "resident_tokens"}]

            symbols = self.values.shapes
            self.view = TensorViewObservation(
                self.values,
                [(mapping, (symbols.symbol("resident_tokens"),))],
                start=symbols.symbol("request_slot") * symbols.symbol("request_map_stride"),
                index=(slot, slice(None, length)),
            )

        with self.view:
            return super().invoke(*args, **kwargs)

    def outputs(self, result: Any) -> Any:
        return tuple(self.output_ports.values())

    def completed(
        self, args: tuple[Any, ...], kwargs: dict[str, Any], result: Any, metadata: dict[str, Any]
    ) -> str | None:
        if self.view.result is None:
            raise ValueError("request cache did not read its declared KV slice")

        self.output_ports = {"request_indices": self.view.result}
        if self.immediate:
            self.output_ports[self.pool_name] = getattr(self.cache.token_to_kv_pool_allocator, self.pool_name)
        if not self.finished:
            self.output_ports["prefix_indices"] = self.request.prefix_indices

        metadata["ports"] = {"inputs": self.input_ports, "outputs": self.values.port_values(self.output_ports)}
        return None


class PageAllocationCall(NativeCall):

    # NPUPagedTokenToKVPoolAllocator.alloc_extend dispatches at this native page-count boundary.
    parallel_page_limit = 200

    def __init__(self, values: Any, original: Any, pool: Any, decode: bool) -> None:
        inputs = {name: {0: "B"} for name in ("seq_lens", "seq_lens_cpu", "last_loc")}
        if not decode:
            inputs.update({name: {0: "B"} for name in ("prefix_lens", "prefix_lens_cpu")})

        super().__init__(values.recording, values, original.__qualname__, original, inputs)
        self.pool = pool
        self.decode = decode
        self.before_count = len(pool.free_pages)

    def resources(self, arguments: dict[str, Any]) -> tuple[Any, Any]:
        self.input_ports = self.values.port_values(
            {
                "free_pages": self.pool.free_pages,
                "sequence_lengths": arguments["seq_lens"],
                "sequence_lengths_cpu": arguments["seq_lens_cpu"],
                "last_locations": arguments["last_loc"],
            }
        )
        if not self.decode:
            self.input_ports.update(
                self.values.port_values(
                    {"prefix_lengths": arguments["prefix_lens"], "prefix_lengths_cpu": arguments["prefix_lens_cpu"]}
                )
            )

        self.values.bind_shape(self.pool.free_pages, (self.values.shapes.symbol("free_pages_before"),))
        return (self.pool.free_pages,), ()

    def invoke(self, *args: Any, **kwargs: Any) -> Any:
        import torch
        from sglang.srt.utils import get_num_new_pages

        with self.values.metadata():
            bound = self.signature.bind(*args, **kwargs)
            bound.apply_defaults()
            arguments = bound.arguments
            provided = arguments.get("num_new_pages")

            # Use the native page-count calculation so source guards match allocator decisions.
            pages = (
                provided
                if provided is not None
                else get_num_new_pages(
                    arguments["seq_lens_cpu"], self.pool.page_size, arguments.get("prefix_lens_cpu"), decode=self.decode
                )
            )

            metadata = self.recording.regions[self.recording.current_scope.get()]["metadata"]
            metadata["entrypoint"] = "kv_allocate"
            metadata["call_family"] = "kv_allocate"
            metadata["parameters"] = {"allocated_pages": pages, "free_pages_before": self.before_count}
            metadata["bindings"] = {"page_size": self.pool.page_size, "need_sort": int(self.pool.need_sort)}
            metadata["guards"] = [
                {"condition": ["le", 0, "allocated_pages"], "expected": True},
                {"condition": ["le", "allocated_pages", "free_pages_before"], "expected": True},
                {"condition": ["eq", "page_size", self.pool.page_size], "expected": True},
                {"condition": "need_sort", "expected": self.pool.need_sort},
            ]

            # Both implementations retain the same allocator ports and state transition.
            if self.decode:
                metadata["guards"].append({"condition": ["eq", "allocated_pages", 0], "expected": pages == 0})
            else:
                metadata["guards"].append(
                    {
                        "condition": ["lt", "allocated_pages", self.parallel_page_limit],
                        "expected": pages < self.parallel_page_limit,
                    }
                )
                metadata["bindings"]["pages_provided"] = int(provided is not None)
                metadata["guards"].append({"condition": "pages_provided", "expected": provided is not None})

            metadata["derived_parameters"] = [
                {"name": "free_pages_after", "expression": ["add", "free_pages_before", ["mul", -1, "allocated_pages"]]}
            ]
            metadata["state_effects"] = {"free_pages": {"before": "free_pages_before", "after": "free_pages_after"}}

            symbols = self.values.shapes
            tail = [(self.pool.free_pages, (symbols.symbol("free_pages_after"),))]
            start = symbols.symbol("allocated_pages")
            members = nullcontext()
            if not self.decode and pages >= self.parallel_page_limit:
                from markov_collection.adapters.frameworks.sglang.allocation import AllocationLoop

                lengths = tuple((arguments["seq_lens_cpu"] - arguments["prefix_lens_cpu"]).tolist())
                contexts = tuple(arguments["prefix_lens_cpu"].tolist())
                members = AllocationLoop(
                    self.values, self.original.__globals__["alloc_extend_naive"], lengths, contexts, self.pool.page_size
                )

        with (
            TensorGeometryObservation(self.values, factories={torch.empty: ("M",)}),
            TensorViewObservation(self.values, tail, start=start),
            members,
        ):
            return super().invoke(*args, **kwargs)

    def completed(
        self, args: tuple[Any, ...], kwargs: dict[str, Any], result: Any, metadata: dict[str, Any]
    ) -> str | None:
        metadata["parameters"]["free_pages_after"] = len(self.pool.free_pages)
        metadata["ports"] = {
            "inputs": self.input_ports,
            "outputs": self.values.port_values({"kv_locations": result, "free_pages": self.pool.free_pages}),
        }
        return None

    def outputs(self, result: Any) -> Any:
        return result, self.pool.free_pages


class PageReleaseCall(NativeCall):
    def __init__(self, recording: Any, values: Any, original: Any, pool: Any) -> None:
        super().__init__(
            recording,
            values,
            "sglang.NPUPagedTokenToKVPoolAllocator.free",
            original,
            {"free_index": {0: "released_tokens"}},
        )

        self.pool_name = "release_pages" if pool.need_sort else "free_pages"
        self.before_count = len(getattr(pool, self.pool_name))
        self.pending_before = (
            sum(indices.numel() for indices in pool.free_group) if not pool.is_not_in_free_group else 0
        )
        self.state_outputs: list[Any] = []

    def invoke(self, *args: Any, **kwargs: Any) -> Any:
        pool, indices = args
        region = self.recording.current_scope.get()
        metadata = self.recording.regions[region]["metadata"]
        metadata["entrypoint"] = "kv_release"
        metadata["entrypoint_phase"] = None

        if not indices.numel() or not pool.is_not_in_free_group:
            return super().invoke(*args, **kwargs)

        with self.values.metadata():
            self.pool_input = self.values.value(getattr(pool, self.pool_name))
            self.values.bind_shape(
                getattr(pool, self.pool_name), (self.values.shapes.symbol(f"{self.pool_name}_before"),)
            )

        # Empty pool operands select a different native concat path; reuse the common grouped interface.
        with UniqueObservation(self.values, "released_pages"), ConcatenationObservation(self.values):
            return super().invoke(*args, **kwargs)

    def completed(
        self, args: tuple[Any, ...], kwargs: dict[str, Any], result: Any, metadata: dict[str, Any]
    ) -> str | None:
        pool, indices = args
        tokens = indices.numel()
        parameters = metadata.setdefault("parameters", {})
        parameters["released_tokens"] = tokens
        metadata["guards"] = [{"condition": ["eq", "released_tokens", 0], "expected": tokens == 0}]
        metadata["ports"] = {
            "inputs": self.values.port_values({"release_indices": indices}),
            "outputs": {},
        }
        if not tokens:
            return None

        parameters["release_immediate"] = int(pool.is_not_in_free_group)
        metadata["guards"].append({"condition": "release_immediate", "expected": pool.is_not_in_free_group})

        # Immediate release updates the page pool; grouped release only accumulates pending tokens.
        if pool.is_not_in_free_group:
            parameters.update(page_size=pool.page_size, need_sort=int(pool.need_sort))
            metadata["guards"].extend(
                [
                    {"condition": ["eq", "page_size", pool.page_size], "expected": True},
                    {"condition": "need_sort", "expected": pool.need_sort},
                ]
            )

            name, before = self.pool_name, self.before_count
            value = getattr(pool, name)
            after, increment = len(value), "released_pages"
            self.values.bind_shape(value, (self.values.shapes.symbol(f"{name}_after"),))
            self.state_outputs.append(value)

            metadata["ports"]["inputs"][name] = self.pool_input
            metadata["ports"]["outputs"] = self.values.port_values({name: value})
        else:
            name, before = "pending_tokens", self.pending_before
            after, increment = sum(value.numel() for value in pool.free_group), "released_tokens"

        # Snapshot values check the native run; the formula supplies the target's resulting state.
        parameters.update({f"{name}_before": before, f"{name}_after": after})
        metadata["derived_parameters"] = [{"name": f"{name}_after", "expression": ["add", f"{name}_before", increment]}]
        metadata["state_effects"] = {name: {"before": f"{name}_before", "after": f"{name}_after"}}
        return None

    def outputs(self, result: Any) -> Any:
        return self.state_outputs


class PageMergeCall(NativeCall):
    def __init__(self, values: Any, original: Any, pool: Any) -> None:
        super().__init__(values.recording, values, "sglang.KV.merge_and_sort_free", original, {})
        self.pool = pool

    def resources(self, arguments: dict[str, Any]) -> tuple[Any, Any]:
        tensors = {name: getattr(self.pool, name) for name in ("free_pages", "release_pages")}
        for name, value in tensors.items():
            self.values.bind_shape(value, (self.values.shapes.symbol(name + "_before"),))
        self.input_ports = self.values.port_values(tensors)
        return tuple(tensors.values()), ()

    def invoke(self, *args: Any, **kwargs: Any) -> Any:
        import torch

        metadata = self.recording.regions[self.recording.current_scope.get()]["metadata"]
        metadata.update(
            entrypoint="kv_merge_free",
            entrypoint_phase=None,
            parameters={
                "free_pages_before": len(self.pool.free_pages),
                "release_pages_before": len(self.pool.release_pages),
            },
            derived_parameters=[
                {"name": "free_pages_after", "expression": ["add", "free_pages_before", "release_pages_before"]}
            ],
            guards=[{"condition": ["lt", 0, "release_pages_before"], "expected": True}],
        )
        with (
            TensorGeometryObservation(self.values, factories={torch.empty: (0,)}),
            ConcatenationObservation(self.values),
        ):
            return super().invoke(*args, **kwargs)

    def outputs(self, result: Any) -> Any:
        return self.pool.free_pages, self.pool.release_pages

    def completed(
        self, args: tuple[Any, ...], kwargs: dict[str, Any], result: Any, metadata: dict[str, Any]
    ) -> str | None:
        metadata["parameters"]["free_pages_after"] = len(self.pool.free_pages)
        self.values.bind_shape(self.pool.free_pages, (self.values.shapes.symbol("free_pages_after"),))
        metadata["ports"] = {
            "inputs": self.input_ports,
            "outputs": self.values.port_values(
                {name: getattr(self.pool, name) for name in ("free_pages", "release_pages")}
            ),
        }
        return None


class PageReleaseGroupCall(NativeCall):
    def __init__(self, recording: Any, values: Any, original: Any, pool: Any) -> None:
        super().__init__(recording, values, "sglang.KV.release_group", original, {})
        self.pool = pool
        self.pool_name = "release_pages" if pool.need_sort else "free_pages"

    def invoke(self, *args: Any, **kwargs: Any) -> Any:
        members = self.pool.free_group
        with self.values.metadata():
            metadata = self.recording.regions[self.recording.current_scope.get()]["metadata"]
            metadata["entrypoint"] = "kv_release_group"
            metadata["entrypoint_phase"] = None
            metadata["parameters"] = {"L": len(members), "released_tokens": sum(value.numel() for value in members)}
            metadata["input_groups"] = {"segments": self.values.input_group(members, 0, "L", "released_tokens")}
            metadata["bindings"] = {
                "release_immediate": 1,
                "page_size": self.pool.page_size,
                "need_sort": int(self.pool.need_sort),
            }

            metadata["ports"] = {
                "inputs": self.values.port_values({self.pool_name: getattr(self.pool, self.pool_name)}),
                "outputs": {},
            }

        with ConcatenationObservation(self.values, "segments"):
            return super().invoke(*args, **kwargs)

    def outputs(self, result: Any) -> Any:
        return getattr(self.pool, self.pool_name)

    def completed(
        self, args: tuple[Any, ...], kwargs: dict[str, Any], result: Any, metadata: dict[str, Any]
    ) -> str | None:
        metadata["ports"]["outputs"] = self.values.port_values({self.pool_name: self.outputs(result)})
        return None
