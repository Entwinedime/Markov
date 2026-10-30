"""Import independent synchronous phase responses, separate from profiled source data."""

from dataclasses import asdict
from decimal import Decimal
import json
from pathlib import Path
from typing import Iterator

from markov.artifacts.performance import Measurement
from markov.artifacts.request import Deployment, positive_integer, read_json


def implementation(configuration: dict, emit: bool) -> str:
    mode = "emit" if emit else "cache_only"
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
        kind = (
            f"inference.{phase}",
            implementation(captured["configuration"], response["emit_token"]),
            "synchronous_phase_interval",
        )
        features = ["query_tokens", "attention_pairs"] if phase == "prefill" else ["attention_pairs"]
        result[kind] = {
            "feature": features + ["first_phase"],
            "nonnegative": True,
            "coverage_features": ["query_tokens", "context_tokens"],
            "limitations": captured["limitations"],
        }
    return result


def measurements(path: Path, captured: dict) -> Iterator[Measurement]:
    position = 0
    for relative in captured["files"]:
        source = path.parent / relative
        with source.open(encoding="utf-8") as stream:
            for line in stream:
                position += 1
                event = json.loads(line)
                fields = event["args"]
                phase = fields["phase"]
                query = positive_integer(fields["query_tokens"], "query_tokens")
                total = positive_integer(fields["context_tokens"], "context_tokens")
                if phase not in ("prefill", "decode") or total < query:
                    raise ValueError("Invalid KTransformers phase geometry")
                duration = int(Decimal(str(event["dur"])) * 1000)
                if duration < 0:
                    raise ValueError("Independent phase duration must be nonnegative")
                (identity,) = fields["request_ids"]
                ordinal = int(identity.removeprefix("request_"))
                yield Measurement(
                    position,
                    "phase_interval_ns",
                    f"inference.{phase}",
                    implementation(captured["configuration"], fields["emit_token"]),
                    "synchronous_phase_interval",
                    {
                        "query_tokens": query,
                        "context_tokens": total,
                        "attention_pairs": query * (2 * total - query + 1) / 2,
                        "first_phase": int(fields["first_phase"]),
                    },
                    duration,
                    ordinal < captured["formal_begin_index"],
                )
