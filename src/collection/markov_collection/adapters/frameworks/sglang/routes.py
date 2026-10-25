"""Check conditional routes against model metadata before native acquisition."""

from __future__ import annotations

from typing import Any


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

        padding = source.get("padding", [])
        if not isinstance(padding, list):
            raise ValueError("padding routes must be an array")
        segments = {}
        for assignment in padding:
            self.insert(assignment, segments, self.padding, False)
        if self.padding:
            self.coverage(segments, self.padding, "padding")

    def insert(
        self, source: dict[str, Any], segments: dict[int, list[tuple[int, int]]], tokens: int, request: bool
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
            segments.setdefault(layer, []).append(interval)

    def coverage(self, segments: dict[int, list[tuple[int, int]]], tokens: int, context: str) -> None:

        # Preflight covers the model range; native module bindings later select the actual expert layers.
        for layer in range(self.layers):
            covered = 0
            for begin, end in sorted(segments.get(layer, [])):
                if begin != covered:
                    raise ValueError(f"route gap or overlap for {context}, layer {layer}")
                covered = end
            if covered != tokens:
                raise ValueError(f"incomplete route for {context}, layer {layer}")

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
