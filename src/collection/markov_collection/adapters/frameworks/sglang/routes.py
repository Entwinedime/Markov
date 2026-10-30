"""Validate and query logical expert choices independently of native batching."""

from __future__ import annotations

from typing import Any

from markov_workload import WorkloadContent


class ExpertRouteConditions:
    def __init__(self, model: dict[str, Any], size: int) -> None:
        self.layers = model["num_hidden_layers"]
        self.experts = model["num_experts"]
        self.top_k = model["num_experts_per_tok"]
        self.padding = size - 1
        if self.experts <= 0 or self.experts % size:
            raise ValueError("EP size must partition the model's routed experts")

    def check(self, source: dict[str, Any], requests: list[dict[str, Any]]) -> None:
        if not isinstance(source, dict) or source.keys() - {"assignments", "padding"}:
            raise ValueError("EP route condition needs assignments and optional padding")
        tokens = {request["id"]: request["prompt_tokens"] + request["output_tokens"] - 1 for request in requests}
        segments = {name: {} for name in tokens}
        assignments = source["assignments"]
        if not isinstance(assignments, list):
            raise ValueError("EP route assignments must be an array")
        for assignment in assignments:
            name = assignment["request"]
            if name not in tokens:
                raise ValueError(f"EP route refers to an unknown request: {name}")
            self.insert(assignment, segments[name], tokens[name], True)
        for name, length in tokens.items():
            self.coverage(segments[name], length, name)
        self.assignments = segments

        padding = source.get("padding", [])
        if not isinstance(padding, list):
            raise ValueError("padding routes must be an array")
        segments = {}
        for assignment in padding:
            self.insert(assignment, segments, self.padding, False)
        if self.padding:
            self.coverage(segments, self.padding, "padding")
        self.padding_routes = segments

    def insert(
        self, source: dict[str, Any], segments: dict[int, list[tuple[int, int, list[int]]]], tokens: int, request: bool
    ) -> None:
        fields = {"layers", "tokens", "experts"} | ({"request"} if request else set())
        if not isinstance(source, dict) or source.keys() - fields:
            raise ValueError("unsupported EP route assignment fields")
        layers = self.interval(source["layers"], self.layers, "layers")
        interval = self.interval(source["tokens"], tokens, "tokens")
        experts = source["experts"]
        if (
            not isinstance(experts, list)
            or len(experts) != self.top_k
            or any(type(expert) is not int or not 0 <= expert < self.experts for expert in experts)
            or len(set(experts)) != len(experts)
        ):
            raise ValueError("route experts must be distinct model expert IDs matching top-k")
        for layer in range(*layers):
            segments.setdefault(layer, []).append((*interval, experts))

    def coverage(self, segments: dict[int, list[tuple[int, int, list[int]]]], tokens: int, context: str) -> None:

        # Preflight covers the model range; native module bindings later select the actual expert layers.
        for layer in range(self.layers):
            covered = 0
            ranges = segments.get(layer, [])
            ranges.sort(key=lambda item: item[0])
            for begin, end, _ in ranges:
                if begin != covered:
                    raise ValueError(f"route gap or overlap for {context}, layer {layer}")
                covered = end
            if covered != tokens:
                raise ValueError(f"incomplete route for {context}, layer {layer}")

    def rows(self, request: str | None, layer: int, begin: int, end: int) -> list[list[int]]:
        if begin == end:
            return []

        layers = self.padding_routes if request is None else self.assignments[request]
        result = []
        for first, last, experts in layers[layer]:
            count = max(0, min(end, last) - max(begin, first))
            result.extend([experts] * count)

        return result

    def representative(self, requests: list[dict[str, Any]], prefixes=()) -> dict[str, Any]:
        assignments = []
        padding = []
        content = WorkloadContent(requests, prefixes)
        width = self.experts // (self.padding + 1)
        for layer in range(self.layers):
            balanced = [
                (expert * self.experts // self.top_k + layer * width) % self.experts for expert in range(self.top_k)
            ]
            choices = (balanced, list(range(self.top_k)), list(range(self.experts - self.top_k, self.experts)))
            for index, request in enumerate(requests):
                shared = content.lengths[request.get("prefix_id")]

                # Shared content supplies real cache hits; private tails retain expert-load diversity.
                if shared:
                    assignments.append(
                        {
                            "request": request["id"],
                            "layers": [layer, layer + 1],
                            "tokens": [0, shared],
                            "experts": balanced,
                        }
                    )
                length = request["prompt_tokens"] + request["output_tokens"] - 1
                if shared == length:
                    continue
                assignments.append(
                    {
                        "request": request["id"],
                        "layers": [layer, layer + 1],
                        "tokens": [shared, length],
                        "experts": choices[(layer + index) % len(choices)],
                    }
                )

            # Match the live tail's routes so its padding does not hide a zero-receiver branch.
            if self.padding:
                padding.append(
                    {
                        "layers": [layer, layer + 1],
                        "tokens": [0, self.padding],
                        "experts": choices[(layer + len(requests) - 1) % len(choices)],
                    }
                )

        return {"assignments": assignments, "padding": padding}

    @staticmethod
    def interval(source: list[int], limit: int, field: str) -> tuple[int, int]:
        if (
            not isinstance(source, list)
            or len(source) != 2
            or any(type(value) is not int for value in source)
            or not 0 <= source[0] < source[1] <= limit
        ):
            raise ValueError(f"route {field} needs an integer [begin, end) range within its logical bounds")
        return source[0], source[1]
