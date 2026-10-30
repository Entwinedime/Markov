"""Import device services interpreted through the same native ownership rules as a base DAG."""

from markov.artifacts.performance import Measurement
from markov.artifacts.request import read_json
from markov.adapters.ascend import tensor_measurements
from markov.adapters.ascend.tensor_responses import validate_recipe


def metadata(path):
    report = read_json(path)
    captured = report["metadata"]
    validate_recipe(captured["recipe"])
    if (
        report["status"] != "completed"
        or captured["status"] != "completed"
        or captured["trace_values_verified"] is not True
        or captured["native_observer_requested"] is not False
    ):
        raise ValueError("Device services require a completed, verified tensor trace without native observation")
    return report


def context(report, deployment):
    return {**tensor_measurements.context(report["metadata"], deployment), "protocol": "profiled_exact_device_services"}


def recipes(report):
    result = {}
    for case in report["metadata"]["recipe"]["cases"]:
        recipe = {
            "feature": "elements",
            "limitations": [
                "Initialized isolated contiguous 1D device services from profiled exact native flows and exclusive DAG intervals.",
                "No contention, memory pressure or cold start coverage; CPU/worker provenance is not a normal service measurement.",
            ],
        }
        if case["operation"] == "concatenate":
            recipe["curve"] = "input_tensors"
        result[tensor_measurements.kind(case, "device_service")] = recipe
    return result


def measurements(path, report):
    recipe = report["metadata"]["recipe"]
    seen, positions = set(), set()
    for row in report["measurements"]:
        case_id, ordinal, position = row["case"], row["ordinal"], row["position"]
        duration = row["device_service_ns"]
        if (
            type(case_id) is not int
            or not 0 <= case_id < len(recipe["cases"])
            or type(ordinal) is not int
            or not 0 <= ordinal < recipe["repeats"]
            or (case_id, ordinal) in seen
            or position in positions
            or type(position) is not int
            or position < 0
            or type(duration) is not int
            or not 0 < duration < 2**63
            or not row["device_records"]
        ):
            raise ValueError(f"Invalid tensor device measurement: {path}")
        seen.add((case_id, ordinal))
        positions.add(position)
        case = recipe["cases"][case_id]
        features = {
            "elements": sum(case["input_sizes"]),
            "input_tensors": len(case["input_sizes"]),
            "element_bytes": 8 if case["dtype"] == "int64" else 4,
            "tensor_rank": 1,
        }
        yield Measurement(
            position, "device_service_ns", *tensor_measurements.kind(case, "device_service"), features, duration, False
        )
    if len(seen) != len(recipe["cases"]) * recipe["repeats"]:
        raise ValueError(f"Incomplete tensor device measurement report: {path}")
