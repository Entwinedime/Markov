"""Validate and query logical expert choices independently of native batching."""

from __future__ import annotations

from typing import Any


class ExpertRoutes:
    def __init__(self, source, requests, layers, model_layers: int, padding_tokens: int) -> None:
        self.layers = {layer["layer"]: layer for layer in layers}
        self.model_layers = model_layers
        self.padding = padding_tokens
        if len(self.layers) != len(layers):
            raise ValueError("conditional routes need unique native expert layers")
        for layer in layers:
            if not 0 <= layer["layer"] < model_layers or not 0 < layer["top_k"] <= layer["experts"]:
                raise ValueError("invalid native expert-layer geometry")
        self.check(source, requests)

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
        layers = self.interval(source["layers"], self.model_layers, "layers")
        interval = self.interval(source["tokens"], tokens, "tokens")
        experts = source["experts"]
        if (
            not isinstance(experts, list)
            or not experts
            or any(type(expert) is not int or expert < 0 for expert in experts)
            or len(set(experts)) != len(experts)
        ):
            raise ValueError("route experts must be distinct model expert IDs matching top-k")
        for layer, geometry in self.layers.items():
            if not layers[0] <= layer < layers[1]:
                continue
            if len(experts) != geometry["top_k"] or any(expert >= geometry["experts"] for expert in experts):
                raise ValueError(f"route experts do not match native layer {layer}")
            segments.setdefault(layer, []).append((*interval, experts))

    def coverage(self, segments: dict[int, list[tuple[int, int, list[int]]]], tokens: int, context: str) -> None:

        # Each worker validates the actual expert layers it owns, including sparse model and PP layouts.
        for layer in self.layers:
            covered = 0
            ranges = segments.get(layer, [])
            ranges.sort(key=lambda item: item[0])
            for begin, end, _ in ranges:
                if begin != covered:
                    raise ValueError(f"route gap or overlap for {context}, layer {layer}")
                covered = end
            if covered != tokens:
                raise ValueError(f"incomplete route for {context}, layer {layer}")

    def batch_segments(self, members, layer: int, padded_tokens: int):
        tokens = sum(member["new_tokens"] for member in members)
        if not tokens <= padded_tokens <= tokens + self.padding:
            raise ValueError("native token padding exceeds the supplied route coverage")
        ranges = [
            (self.assignments[member["id"]][layer], member["context_tokens"], member["context_tokens"] + member["new_tokens"])
            for member in members
        ]
        if padded_tokens > tokens:
            ranges.append((self.padding_routes[layer], 0, padded_tokens - tokens))
        for segments, begin, end in ranges:
            if not 0 <= begin <= end <= segments[-1][1]:
                raise ValueError("native token interval exceeds the supplied route coverage")
            for first, last, experts in segments:
                count = max(0, min(end, last) - max(begin, first))
                if count:
                    yield count, experts

    def batch_rows(self, members, layer: int, padded_tokens: int) -> list[list[int]]:
        result = []
        for count, experts in self.batch_segments(members, layer, padded_tokens):
            result.extend([experts] * count)
        return result

    def batch_counts(self, members, layer: int, padded_tokens: int) -> list[int]:
        counts = [0] * self.layers[layer]["experts"]

        # Prediction visits route intervals, not an expanded token-by-expert matrix.
        for count, experts in self.batch_segments(members, layer, padded_tokens):
            for expert in experts:
                counts[expert] += count
        return counts

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
