"""Keep whole API responses distinct from worker and device service measurements."""

import json
from pathlib import Path

from markov.artifacts.performance import Measurement
from markov.artifacts.request import read_json
from markov.adapters.ascend.tensor_responses import validate_recipe


QUANTITIES = {
    "caller_wall_ns": "caller_blocking_interval",
    "caller_thread_cpu_ns": "caller_thread_cpu_interval",
    "completion_observed_ns": "completion_observation_upper_interval",
}


def kind(case, scope):
    clone = case["operation"] == "clone"
    return (
        "tensor.clone" if clone else "tensor.concatenate",
        f"aten.{'clone' if clone else 'cat'}:npu:{case['dtype']}",
        scope,
    )


def metadata(path: Path):
    captured = read_json(path)
    if captured["status"] != "completed":
        raise ValueError(f"Tensor response capture did not complete: {path}")
    validate_recipe(captured["recipe"])
    if type(captured["native_observer_requested"]) is not bool:
        raise ValueError("Tensor response capture must declare whether native observation was requested")
    return captured


def context(captured, deployment):
    if deployment["runtime"] != "ascend":
        raise ValueError("Tensor responses require the Ascend runtime")
    return {
        "platform": deployment["platform"],
        "runtime": deployment["runtime"],
        **{key: captured[key] for key in ("torch", "torch_npu", "device_name", "native_observer_requested")},
    }


def recipes(captured):
    if captured["native_observer_requested"]:
        raise ValueError("Instrumented tensor responses can be stored, but need overhead treatment before training")
    result = {}
    for case in captured["recipe"]["cases"]:
        for scope in QUANTITIES.values():
            recipe = {
                "feature": "elements",
                "limitations": [
                    "Initialized isolated contiguous 1D ATen API; not cold start, competing load or memory pressure.",
                    "Caller/whole-completion responses do not identify enqueue, worker launch/retirement or bare device service. "
                    "These scopes cannot replace a queued operation's CPU, worker and device services or be summed together.",
                ],
            }
            if case["operation"] == "concatenate":
                recipe["curve"] = "input_tensors"
            result[kind(case, scope)] = recipe
    return result


def measurements(path: Path, captured):
    recipe = captured["recipe"]
    repetitions = recipe["warmup"] + recipe["repeats"]
    seen = set()
    with (path.parent / "measurements.jsonl").open(encoding="utf-8") as stream:
        for position, line in enumerate(stream, start=1):
            row = json.loads(line)
            case_index, ordinal = row["case"], row["ordinal"]
            if type(case_index) is not int or not 0 <= case_index < len(recipe["cases"]):
                raise ValueError(f"Unknown tensor response case: {path}, line {position}")
            case = recipe["cases"][case_index]
            if (
                any(row[key] != case[key] for key in ("operation", "dtype", "input_sizes"))
                or type(ordinal) is not int
                or not 0 <= ordinal < repetitions
                or (case_index, ordinal) in seen
                or row["values_verified"] is not True
                or type(row["warmup"]) is not bool
                or row["warmup"] != (ordinal < recipe["warmup"])
            ):
                raise ValueError(f"Invalid verified tensor response: {path}, line {position}")
            seen.add((case_index, ordinal))
            features = {
                "elements": sum(case["input_sizes"]),
                "input_tensors": len(case["input_sizes"]),
                "element_bytes": 8 if case["dtype"] == "int64" else 4,
                "tensor_rank": 1,
            }
            for quantity, scope in QUANTITIES.items():
                duration = row[quantity]
                if type(duration) is not int or not 0 <= duration < 2**63:
                    raise ValueError(f"Invalid tensor response nanoseconds: {path}, line {position}, {quantity}")
                yield Measurement(position, quantity, *kind(case, scope), features, duration, row["warmup"])
    if len(seen) != len(recipe["cases"]) * repetitions or not seen:
        raise ValueError(f"Tensor capture does not cover its declared cases and repetitions: {path}")
