"""Declared warmup and measurement identity coverage."""

from typing import Any


class SamplingDomain:
    def __init__(self, prepared: dict[str, Any]) -> None:
        self.measurement_ids = set(prepared["measurement_ids"])
        self.warmup_ids = set(prepared["warmup_ids"])
        self._warmed: set[tuple[Any, ...]] = set()
        self._missing: dict[tuple[Any, ...], dict[str, Any]] = {}
        self._warmup_records = 0
        self._measurement_records = 0

    def stage(self, metadata: dict[str, Any]) -> str:
        ids = {request["id"] for request in metadata["requests"]}
        if ids <= self.warmup_ids:
            return "warmup"
        if ids <= self.measurement_ids:
            return "measurement"

        raise ValueError("native sampling batch mixes or contains undeclared measurement identities")

    def observe(self, rank: int, step: str, metadata: dict[str, Any]) -> None:
        features = ("input_tokens", "context_tokens", "prompt_tokens", "generated_tokens")
        requests = [{name: request[name] for name in features} for request in metadata["requests"]]
        key = (rank, step, metadata["phase"], tuple(tuple(request.values()) for request in requests))

        if self.stage(metadata) == "warmup":
            self._warmed.add(key)
            self._warmup_records += 1
        else:
            self._measurement_records += 1
            if key not in self._warmed:
                self._missing[key] = {"rank": rank, "step": step, "phase": metadata["phase"], "requests": requests}

    def coverage(self) -> dict[str, Any]:
        return {
            "warmup_records": self._warmup_records,
            "measurement_records": self._measurement_records,
            "missing_conditions": list(self._missing.values()),
        }
