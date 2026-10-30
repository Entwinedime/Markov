"""Import independent synchronous phase responses, separate from profiled source data."""

from dataclasses import asdict
from decimal import Decimal
import json
from pathlib import Path
from typing import Iterator

from markov.artifacts.performance import Measurement
from markov.artifacts.request import Deployment, positive_integer, read_json


def implementation(configuration: dict, mode: str) -> str:
    return (
        f"ktransformers.dense.torch:{configuration['attention_backend']}:{mode}:threads={configuration['cpu_threads']}"
    )


def metadata(path: Path) -> dict:
    captured = read_json(path)
    if (
        captured["kind"] != "independent_ktransformers_regions"
        or captured["status"] != "completed"
        or captured["profiling"] is not False
        or not captured["files"]
    ):
        raise ValueError("KTransformers performance input must be a completed independent capture")
    return captured


def context(captured: dict, deployment: dict) -> dict:
    current = asdict(Deployment.parse(captured["deployment"]))
    if current != deployment:
        raise ValueError("Independent KTransformers measurements use a different deployment")
    return current


def recipes(captured: dict) -> dict:
    result = {}
    for response in captured["responses"]:
        phase = response["phase"]
        control = phase == "control"
        if control:
            mode = "control"
            features = ["input_tokens", "prefill_steps", "decode_steps", "first_request"]
        else:
            mode = "emit" if response["emit_token"] else "cache_only"
            features = ["query_tokens", "attention_pairs"] if phase == "prefill" else ["attention_pairs"]
            features.append("first_phase")
        kind = (
            "inference.request_control" if control else f"inference.{phase}",
            implementation(captured["configuration"], mode),
            "exclusive_request_interval" if control else "synchronous_phase_interval",
        )
        result[kind] = {
            "feature": features,
            "nonnegative": True,
            "coverage_features": [] if control else ["query_tokens", "context_tokens"],
            "limitations": captured["limitations"],
        }
    return result


def measurements(path: Path, captured: dict) -> Iterator[Measurement]:
    position = 0
    phase_durations = {}
    for relative in captured["files"]:
        source = path.parent / relative
        with source.open(encoding="utf-8") as stream:
            for line in stream:
                position += 1
                event = json.loads(line)
                fields = event["args"]
                (identity,) = fields["request_ids"]
                ordinal = int(identity.removeprefix("request_"))
                duration = int(Decimal(str(event["dur"])) * 1000)
                control = event["name"] == "runtime.ktransformers.generate"
                if control:
                    mode = "control"
                    duration -= phase_durations.pop(identity)
                    features = {
                        key: int(fields[key])
                        for key in ("input_tokens", "prefill_steps", "decode_steps", "first_request")
                    }
                    phase = "request_control"
                else:
                    mode = "emit" if fields["emit_token"] else "cache_only"
                    phase = fields["phase"]
                    query = positive_integer(fields["query_tokens"], "query_tokens")
                    total = positive_integer(fields["context_tokens"], "context_tokens")
                    if phase not in ("prefill", "decode") or total < query:
                        raise ValueError("Invalid KTransformers phase geometry")
                    phase_durations[identity] = phase_durations.get(identity, 0) + duration
                    features = {
                        "query_tokens": query,
                        "context_tokens": total,
                        "attention_pairs": query * (2 * total - query + 1) / 2,
                        "first_phase": int(fields["first_phase"]),
                    }
                if duration < 0:
                    raise ValueError("Independent exclusive duration must be nonnegative")
                yield Measurement(
                    position,
                    "exclusive_request_ns" if control else "phase_interval_ns",
                    f"inference.{phase}",
                    implementation(captured["configuration"], mode),
                    "exclusive_request_interval" if control else "synchronous_phase_interval",
                    features,
                    duration,
                    ordinal < captured["formal_begin_index"],
                )
