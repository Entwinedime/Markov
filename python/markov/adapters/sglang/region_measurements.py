"""Import small independent region captures; never read target scores or base traces."""

from dataclasses import asdict
from collections.abc import Iterator
from decimal import Decimal
import json
from pathlib import Path

from markov.artifacts.performance import Measurement
from markov.artifacts.request import Deployment, read_json
from markov.adapters.sglang.region_probe import IMPLEMENTATION


def result_implementation(captured: dict) -> str:
    cache = "HiRadixCache" if captured["configuration"]["enable_hierarchical_cache"] else "RadixCache"
    return IMPLEMENTATION + ":" + cache


def metadata(path: Path) -> dict:
    captured = read_json(path)
    if (
        captured["kind"] != "independent_region_measurements"
        or captured["status"] != "completed"
        or captured["profiling"] is not False
        or not captured["files"]
    ):
        raise ValueError(f"Independent region capture is incomplete: {path}")
    return captured


def context(captured: dict, deployment: dict) -> dict:
    current = asdict(Deployment.parse(captured["deployment"]))
    if current != deployment:
        raise ValueError("Region measurement deployment differs from the selected model deployment")
    return current


def recipes(captured: dict) -> dict:
    result = {}
    for phase in captured["phases"]:
        for operation, scope in (
            ("prepare", "thread_cpu"),
            ("prepare", "caller_blocking_interval"),
            ("prepare", "current_stream_interval"),
            ("batch", "thread_cpu"),
            ("batch", "caller_blocking_interval"),
            ("batch", "current_stream_interval"),
            ("result", "thread_cpu"),
        ):
            predictors = ["query_tokens", "attention_pairs"] if phase == "prefill" else ["attention_pairs"]
            if operation != "result":
                predictors += ["first_phase"]
            if operation == "prepare" and phase == "prefill":
                predictors += ["new_preparations"]
            if operation == "result":
                predictors += ["finished"]
            implementation = result_implementation(captured) if operation == "result" else IMPLEMENTATION
            result[(f"{operation}.{phase}", implementation, scope)] = {
                "feature": predictors,
                "curve": "page_tokens",
                "nonnegative": True,
                "coverage_features": ["context_tokens", "batch_size", "query_tokens"]
                + (["new_preparations"] if operation == "prepare" else [])
                + (["finished"] if operation == "result" else []),
                "limitations": captured["limitations"],
            }
    final_phases = set("prefill" if count == 1 else "decode" for count in captured["output_counts"])
    for phase in final_phases:
        for part in ("send", "tail"):
            result[(f"result_{part}.{phase}", result_implementation(captured), "thread_cpu")] = {
                "feature": "attention_pairs",
                "curve": "page_tokens",
                "nonnegative": True,
                "coverage_features": ["context_tokens", "batch_size", "query_tokens", "finished"],
                "limitations": captured["limitations"],
            }
    return result


def events(path: Path, captured: dict) -> Iterator[tuple[int, dict, bool]]:
    position = 0
    formal_begin = captured["formal_begin_index"]
    for relative in captured["files"]:
        source = (path.parent / relative).resolve()
        if not source.is_relative_to(path.parent.resolve()):
            raise ValueError(f"Region measurement file escapes its capture: {relative}")
        with source.open(encoding="utf-8") as stream:
            for line in stream:
                position += 1
                event = json.loads(line)
                fields = event["args"]
                if fields.get("status", "returned") != "returned":
                    raise ValueError(f"Failed region measurement: {source}, line {position}")
                if fields["implementation"] != IMPLEMENTATION or fields["phase"] not in ("prefill", "decode"):
                    raise ValueError(f"Unsupported region implementation: {source}, line {position}")
                warmup = all(int(rid.removeprefix("request_")) < formal_begin for rid in fields["request_ids"])
                yield position, event, warmup


def measurements(path: Path, captured: dict) -> Iterator[Measurement]:
    for position, event, warmup in events(path, captured):
        if event["name"] in ("region.scheduler_idle", "region.load_failed", "prefetch.query"):
            continue
        fields = event["args"]
        name = event["name"].removeprefix("region.")
        if name in ("stream", "prepare_stream"):
            kind = ("batch" if name == "stream" else "prepare", "current_stream_interval")
            duration = fields["stream_interval_ns"]
        elif name in ("prepare", "batch", "result"):
            kind = (name, "thread_cpu")
            duration = fields["thread_cpu_ns"]
        else:
            raise ValueError(f"Unknown region measurement: {event['name']}")
        if type(duration) is not int or duration < 0:
            raise ValueError(f"Invalid region duration in {path}, record {position}")
        features = {**fields["features"]}
        if kind[0] != "prepare":
            features.pop("new_preparations")
        if kind[0] == "result":
            features.pop("first_phase")
        implementation = result_implementation(captured) if name == "result" else IMPLEMENTATION
        yield Measurement(
            position,
            kind[1],
            f"{kind[0]}.{fields['phase']}",
            implementation,
            kind[1],
            features,
            duration,
            warmup,
        )
        if name in ("prepare", "batch"):
            wall = int(Decimal(str(event["dur"])) * 1000)
            yield Measurement(
                position,
                "caller_interval_ns",
                f"{name}.{fields['phase']}",
                IMPLEMENTATION,
                "caller_blocking_interval",
                features,
                wall,
                warmup,
            )
        if name == "result" and "send_cpu_ns" in fields:
            for part, cost in (("send", fields["send_cpu_ns"]), ("tail", duration - fields["send_cpu_ns"])):
                if cost < 0:
                    raise ValueError(f"Result CPU ports exceed their parent interval: {path}, record {position}")
                yield Measurement(
                    position,
                    part + "_thread_cpu",
                    f"result_{part}.{fields['phase']}",
                    implementation,
                    "thread_cpu",
                    features,
                    cost,
                    warmup,
                )
