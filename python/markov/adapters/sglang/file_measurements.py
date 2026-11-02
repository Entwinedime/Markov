"""Import independent file worker responses observed during native idle polling."""

from collections import Counter
from collections.abc import Iterator
import json
from pathlib import Path

from markov.adapters.sglang.cache_measurements import file_recipes
from markov.adapters.sglang import prefetch_measurements
from markov.adapters.sglang.region_measurements import context as context
from markov.artifacts.performance import Measurement
from markov.artifacts.request import read_json


def metadata(path: Path) -> dict:
    captured = read_json(path)
    if captured["kind"] != "independent_file_worker_measurements" or captured["status"] != "completed":
        raise ValueError("File worker measurements require a complete independent capture")
    if captured["profiling"]:
        raise ValueError("Profiled file worker observations cannot train normal responses")
    return captured


def recipes(captured: dict) -> dict:
    result = file_recipes(
        [
            "Serial native file-prefetch workload with the declared TP deployment and normal foreground polling.",
            "Worker intervals include interpreter scheduling and blocking; they are not exclusive CPU or bare storage bandwidth.",
            "Only background file calls are timed; no compute-region NPU events, profiler or per-poll recording.",
            "Busy serving, other foreground policies and unrelated host load remain unqualified.",
        ]
    )
    result[(prefetch_measurements.OPERATION, prefetch_measurements.IMPLEMENTATION, prefetch_measurements.SCOPE)] = {
        "feature": ["hashed_pages", "checked_pages"],
        "curve": "page_tokens",
        "nonnegative": True,
        "extrapolation_features": ["hashed_pages", "checked_pages"],
        "limitations": [
            "Closed background query from eligible enqueue through native rank reduction, excluding prior query backlog.",
            "Normal foreground polling, warm local file metadata and the declared TP deployment; no NPU event probes.",
            "One shared query lane approximates rank-return skew; unrelated host load and concurrent file changes remain unqualified.",
        ],
    }
    return result


def measurements(path: Path, captured: dict) -> Iterator[Measurement]:
    seen = Counter()
    position = 0
    queries = []
    for relative in captured["files"]:
        source = (path.parent / relative).resolve()
        if not source.is_relative_to(path.parent.resolve()):
            raise ValueError(f"File worker observation escapes its capture: {relative}")
        with source.open(encoding="utf-8") as stream:
            for line in stream:
                position += 1
                event = json.loads(line)
                fields = event["args"]
                if event["name"] == "prefetch.query":
                    warmup = all(
                        int(rid.removeprefix("request_")) < captured["formal_begin_index"]
                        for rid in fields["request_ids"]
                    )
                    queries.append((position, event, warmup))
                    continue
                key = fields["participant"], event["name"]
                seen[key] += 1
                warmup = seen[key] <= captured["warmup_batches"]

                for index, cost in enumerate(fields["costs"]):
                    duration = cost["duration_ns"]
                    if type(duration) is not int or duration < 0:
                        raise ValueError(f"Invalid file worker interval: {source}, record {position}")
                    yield Measurement(
                        position,
                        f"{cost['operation']}:{index}",
                        cost["operation"],
                        fields["implementation"],
                        "storage_worker_service",
                        {
                            "pages": cost["pages"],
                            "page_bytes": fields["page_bytes"],
                        },
                        duration,
                        warmup,
                    )
    yield from prefetch_measurements.response_measurements(iter(queries), captured)
