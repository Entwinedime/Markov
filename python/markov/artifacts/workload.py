"""Bounded serial requests, shared by capture and later prediction preparation."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from markov.artifacts.request import positive_integer, read_json, reject_unknown, required_text


@dataclass(frozen=True)
class RequestInput:
    prompt: str | None
    input_ids: list[int] | None
    max_new_tokens: int
    ignore_eos: bool
    return_logprob: bool

    @classmethod
    def parse(cls, raw: dict):
        reject_unknown(
            raw, {"prompt", "input_ids", "max_new_tokens", "ignore_eos", "return_logprob"}, "workload request"
        )
        if ("prompt" in raw) == ("input_ids" in raw):
            raise ValueError("Each workload request needs exactly one of prompt or input_ids")
        prompt = required_text(raw["prompt"], "prompt") if "prompt" in raw else None
        ids = raw.get("input_ids")
        if "input_ids" in raw and (
            not isinstance(ids, list) or not ids or any(type(token) is not int or token < 0 for token in ids)
        ):
            raise ValueError("input_ids must be a nonempty array of nonnegative integers")
        for key in ("ignore_eos", "return_logprob"):
            if key in raw and type(raw[key]) is not bool:
                raise ValueError(f"{key} must be boolean")
        return cls(
            prompt,
            ids,
            positive_integer(raw["max_new_tokens"], "max_new_tokens"),
            raw.get("ignore_eos", False),
            raw.get("return_logprob", False),
        )


@dataclass(frozen=True)
class Workload:
    requests: tuple[RequestInput, ...]
    formal_begin_index: int

    @classmethod
    def load(cls, path: Path):
        raw = read_json(path)
        reject_unknown(raw, {"requests", "arrival", "formal_begin_index"}, "workload")
        if raw["arrival"] != "serial":
            raise ValueError("Only serial request arrival is supported")
        requests = tuple(RequestInput.parse(row) for row in raw["requests"])
        begin = raw.get("formal_begin_index", 0)
        if not requests or type(begin) is not int or not 0 <= begin < len(requests):
            raise ValueError("formal_begin_index must select a request in a nonempty workload")
        return cls(requests, begin)
