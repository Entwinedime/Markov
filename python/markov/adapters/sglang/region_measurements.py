"""Import small independent region captures; never read target scores or base traces."""

from collections import defaultdict
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
        for part in ("before_prepare", "before_batch", "before_result", "broadcast"):
            if "region.control." + part not in captured.get("measurement_ports", []):
                continue
            group = part in ("before_prepare", "broadcast")
            features = (
                ["requests", "input_tokens"] if part == "broadcast"
                else ["query_tokens", "context_tokens"] if part == "before_prepare" and phase == "prefill"
                else None
            )
            scope = "controller_group_response" if group else "caller_blocking_interval"
            result[(f"control.{part}.{phase}", IMPLEMENTATION, scope)] = {
                "feature": features,
                "curve": "page_tokens",
                "nonnegative": True,
                "coverage_features": [] if part == "broadcast" else ["query_tokens", "context_tokens", "batch_size"],
                "limitations": [
                    "Independent initialized serial frontend response; exclusive CPU service and contention are not identified.",
                    "Group timing starts after the last observed entry. Probe work outside the selected ports remains uncertain.",
                ],
            }
        for operation, scope in (
            ("prepare", "thread_cpu"),
            ("prepare", "caller_blocking_interval"),
            ("prepare", "current_stream_interval"),
            ("batch", "input_ready_thread_cpu"),
            ("batch", "input_ready_caller_interval"),
            ("batch", "current_stream_interval"),
            ("result", "thread_cpu"),
        ):
            if (
                operation == "batch"
                and scope != "current_stream_interval"
                and "region.stream.input_metadata" not in captured.get("measurement_ports", [])
            ):
                continue

            predictors = ["query_tokens", "attention_pairs"] if phase == "prefill" else ["attention_pairs"]
            if operation != "result":
                predictors += ["first_phase"]
            if operation != "result" and phase == "prefill":
                predictors += ["new_preparations"]
            if operation == "result":
                predictors += ["finished"]
            implementation = result_implementation(captured) if operation == "result" else IMPLEMENTATION
            recipe = {
                "feature": predictors,
                "curve": "page_tokens",
                "nonnegative": True,
                "coverage_features": ["context_tokens", "batch_size", "query_tokens"]
                + (["new_preparations"] if operation == "prepare" else [])
                + (["finished"] if operation == "result" else []),
                "limitations": captured["limitations"],
            }
            if operation != "result":
                recipe["initialization_features"] = ["first_phase"]
                if phase == "prefill":
                    recipe["initialization_features"].append("new_preparations")
            if operation == "batch" and phase == "prefill" and scope == "current_stream_interval":
                recipe["hinge"] = "query_tokens"
            result[(f"{operation}.{phase}", implementation, scope)] = recipe
    final_phases = set("prefill" if count == 1 else "decode" for count in captured["output_counts"])
    for phase in final_phases:
        for part in ("send", "tail"):
            result[(f"result_{part}.{phase}", result_implementation(captured), "thread_cpu")] = {
                "feature": None,
                "curve": "page_tokens",
                "nonnegative": True,
                "coverage_features": ["attention_pairs", "context_tokens", "batch_size", "query_tokens", "finished"],
                "limitations": captured["limitations"]
                + ["Typical serial small-output CPU response; payload-size scaling is not modeled."],
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
    records = list(events(path, captured))
    metadata_by_frame = {
        (event["pid"], tuple(event["args"]["request_ids"]), event["args"]["iteration"]): event["args"]["input_metadata"]
        for _, event, _ in records
        if event["name"] == "region.stream" and "stream_interval_ns" in event["args"].get("input_metadata", {})
    }
    for position, event, warmup in records:
        if event["name"].startswith("region.control."):
            continue
        if event["name"] in (
            "region.scheduler_idle",
            "region.load_failed",
            "prefetch.query",
            "prefetch.issue",
            "prefetch.check",
            "prefetch.accept",
        ):
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
        if kind[0] == "result" or (kind[0] == "batch" and fields["phase"] != "prefill"):
            features.pop("new_preparations")
        if kind[0] == "result":
            features.pop("first_phase")
        implementation = result_implementation(captured) if name == "result" else IMPLEMENTATION
        wall = int(Decimal(str(event["dur"])) * 1000) if name in ("prepare", "batch") else None
        if name == "batch":
            metadata = metadata_by_frame.get((event["pid"], tuple(fields["request_ids"]), fields["iteration"]))
            if metadata is None:
                continue

            # Host-minus-device timing estimates wait/spin, but can include
            # host setup and event queueing; it is not a strict bound.
            device_ns = metadata["stream_interval_ns"]
            duration -= max(0, metadata["thread_cpu_ns"] - device_ns)
            wall -= max(0, metadata["wall_ns"] - device_ns)
            kind = (name, "input_ready_thread_cpu")
            if device_ns < 0 or duration < 0 or wall < 0:
                raise ValueError(f"Input metadata exceeds its batch interval: {path}, record {position}")

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
            scope = "input_ready_caller_interval" if name == "batch" else "caller_blocking_interval"
            yield Measurement(
                position,
                scope if name == "batch" else "caller_interval_ns",
                f"{name}.{fields['phase']}",
                IMPLEMENTATION,
                scope,
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

    yield from control_measurements(records, captured)


def control_measurements(records: list, captured: dict) -> Iterator[Measurement]:
    groups = defaultdict(list)
    for position, event, warmup in records:
        if not event["name"].startswith("region.control."):
            continue

        fields = event["args"]
        part = event["name"].removeprefix("region.control.")
        phase = fields["phase"]
        features = dict(fields["features"])
        if part != "broadcast":
            features = {key: features[key] for key in ("query_tokens", "context_tokens", "page_tokens", "batch_size")}
        grouped = part in ("before_prepare", "broadcast")
        scope = "controller_group_response" if grouped else "caller_blocking_interval"
        sample_warmup = warmup or bool(fields["features"].get("first_phase", False))
        if grouped:
            identity = part, phase, tuple(fields["request_ids"]), fields["iteration"]
            groups[identity].append((position, event, sample_warmup, features))
            continue

        duration = int(Decimal(event["dur"]) * 1000)
        if duration < 0:
            raise ValueError("Frontend response duration must be nonnegative")
        yield Measurement(position, scope, f"control.{part}.{phase}", IMPLEMENTATION, scope, features, duration, sample_warmup)

    for (part, phase, _, _), rows in groups.items():
        ranks = {row[1]["args"]["participant"] for row in rows}
        if ranks != set(range(captured["deployment"]["tensor_parallel"])) or len(ranks) != len(rows):
            raise ValueError("Frontend group response needs one observation per TP participant")
        if any(row[3] != rows[0][3] for row in rows):
            raise ValueError("Frontend participants disagree on the group work")

        begin = max(int(Decimal(row[1]["ts"]) * 1000) for row in rows)
        end = max(int((Decimal(row[1]["ts"]) + Decimal(row[1]["dur"])) * 1000) for row in rows)
        if end < begin:
            raise ValueError("Frontend group response has no common arrival interval")
        scope = "controller_group_response"
        yield Measurement(rows[0][0], scope, f"control.{part}.{phase}", IMPLEMENTATION, scope,
                          rows[0][3], end - begin, all(row[2] for row in rows))
