"""Translate verified normal cache-controller samples into independent cost facts."""

import json
from pathlib import Path
from typing import Iterator

from markov.artifacts.performance import Measurement
from markov.artifacts.request import positive_integer, read_json


IMPLEMENTATION = "sglang.HiCacheController.write:kernel_ascend.page_first_direct"
LOAD_IMPLEMENTATION = "sglang.HiCacheController.start_loading:kernel_ascend.page_first_direct"
LOAD_PREPARATION = "sglang.HiRadixCache.load_back:kernel_ascend.page_first_direct"
RESPONSES = {
    "backup": {
        "caller_return_ns": ("cache.backup.caller", IMPLEMENTATION, "caller_blocking_interval"),
        "completion_observed_ns": ("cache.backup.completion", IMPLEMENTATION, "completion_observation_upper_interval"),
        "host_index_clone_ns": ("cache.index_clone", "aten.clone:cpu:int64", "caller_blocking_interval"),
    },
    "load": {
        "load_prepare_return_ns": ("cache.load.prepare", LOAD_PREPARATION, "caller_blocking_interval"),
        "load_caller_return_ns": ("cache.load.caller", LOAD_IMPLEMENTATION, "caller_blocking_interval"),
        "load_completion_observed_ns": (
            "cache.load.completion",
            LOAD_IMPLEMENTATION,
            "completion_observation_upper_interval",
        ),
    },
    "file": {
        "file_allocate_ns": ("cache.prefetch.allocate", "file.generic_page_get", "storage_worker_service"),
        "file_read_page_ns": ("cache.prefetch.read_page", "file.generic_page_get", "storage_worker_service"),
        "file_copy_publish_ns": ("cache.prefetch.page_publish", "file.generic_page_get", "storage_worker_service"),
        "file_return_ns": ("cache.prefetch.return", "file.generic_page_get", "storage_worker_service"),
        "file_new_gather_ns": (
            "cache.storage_write.gather",
            "file.materialize_then_batch_set",
            "storage_worker_service",
        ),
        "file_existing_gather_ns": (
            "cache.storage_write.gather",
            "file.materialize_then_batch_set",
            "storage_worker_service",
        ),
        "file_new_batch_prepare_ns": (
            "cache.storage_write.batch_prepare",
            "file.materialize_then_batch_set",
            "storage_worker_service",
        ),
        "file_existing_batch_prepare_ns": (
            "cache.storage_write.batch_prepare",
            "file.materialize_then_batch_set",
            "storage_worker_service",
        ),
        "file_new_page_ns": (
            "cache.storage_write.new_page",
            "file.materialize_then_batch_set",
            "storage_worker_service",
        ),
        "file_existing_page_ns": (
            "cache.storage_write.existing_page",
            "file.materialize_then_batch_set",
            "storage_worker_service",
        ),
    },
}
FILE_PER_PAGE = {"file_read_page_ns", "file_copy_publish_ns", "file_new_page_ns", "file_existing_page_ns"}


def recipes(captured: dict) -> dict:
    limits = [
        "Independent initialized idle single-device response; no competing I/O or CPU load. "
        "The declared deployment is supplied by the maintainer, not inferred from a capture's device index."
    ]
    limits.append(captured["limitations"])
    result = {}
    for direction in captured["recipe"].get("operations", ["backup"]):
        for quantity, kind in RESPONSES[direction].items():
            if direction == "file":
                result[kind] = {
                    "feature": None if quantity in FILE_PER_PAGE else "pages",
                    "curve": "page_bytes",
                    "nonnegative": True,
                    "extrapolation_features": ["pages"],
                    "limitations": limits
                    + [
                        "Native buffered file responses on warm filesystem cache; no fsync or cold-media guarantee.",
                        "Allocation includes read-batch iteration overhead; batch-prepare includes write-loop/return overhead. Recording overhead remains included.",
                    ],
                }
                continue
            if kind[0] == "cache.index_clone":
                result[kind] = {
                    "feature": "elements",
                    "nonnegative": True,
                    "limitations": limits
                    + ["One complete host API response, not separately measured CPU self service."],
                }
                continue
            preparation = kind[0] == "cache.load.prepare"
            result[kind] = {
                "feature": ["pages", "segments"] if preparation else "pages",
                "curve": "page_bytes",
                "nonnegative": True,
                "extrapolation_features": ["pages"],
                "limitations": limits
                + [
                    "Initialized opaque controller response; page-count extrapolation assumes fixed setup and repeated per-page work.",
                    "Completion includes its observation API, not bare DMA. Coarse load execution waits for all layers and omits layer/compute overlap.",
                ],
            }
    return result


def metadata(path: Path) -> dict:
    captured = read_json(path)
    if captured["status"] != "completed":
        raise ValueError(f"Independent capture did not complete: {path}")
    if (captured["dtype"], captured["backend"], captured["host_layout"]) != (
        "bfloat16",
        "kernel_ascend",
        "page_first_direct",
    ):
        raise ValueError(f"Unsupported cache measurement implementation: {path}")
    return captured


def context(captured: dict, deployment: dict) -> dict:
    if (deployment["framework"], deployment["runtime"], deployment["dtype"]) != ("sglang", "ascend", captured["dtype"]):
        raise ValueError("Cache measurements require a matching SGLang/Ascend deployment and dtype")

    # Single-device responses depend on local work, not model identity or global TP.
    return {
        "platform": deployment["platform"],
        "framework": deployment["framework"],
        "runtime": deployment["runtime"],
        **{key: captured[key] for key in ("torch", "dtype", "backend", "host_layout")},
    }


def measurements(path: Path, captured: dict) -> Iterator[Measurement]:
    geometry = captured["recipe"]["geometry"]
    for name in ("layers", "kv_heads", "head_dim"):
        positive_integer(geometry[name], name)
    bytes_per_token = 4 * geometry["layers"] * geometry["kv_heads"] * geometry["head_dim"]
    recipe = captured["recipe"]
    warmup = positive_integer(recipe["warmup"], "warmup")
    repetitions = warmup + positive_integer(recipe["repeats"], "repeats")
    cases = {
        (positive_integer(c["page_tokens"], "page_tokens"), positive_integer(c["pages"], "pages"), c.get("segments", 1))
        for c in recipe["cases"]
    }
    directions = recipe.get("operations", ["backup"])
    seen = set()
    with (path.parent / "measurements.jsonl").open(encoding="utf-8") as stream:
        for position, line in enumerate(stream, start=1):
            row = json.loads(line)
            pages = positive_integer(row["pages"], "pages")
            tokens = positive_integer(row["page_tokens"], "page_tokens")
            segments = row.get("segments", 1)
            direction = row.get("operation", "backup")
            ordinal = row["ordinal"]
            identity = (tokens, pages, segments, direction, ordinal)
            if (
                (tokens, pages, segments) not in cases
                or direction not in directions
                or type(ordinal) is not int
                or not 0 <= ordinal < repetitions
                or identity in seen
                or row["physical_pages_verified"] != pages
                or type(row["warmup"]) is not bool
                or row["warmup"] != (ordinal < warmup)
            ):
                raise ValueError(f"Invalid verified measurement: {path}, line {position}")
            seen.add(identity)
            work = {
                "pages": pages,
                "page_bytes": tokens * bytes_per_token,
                "layers": geometry["layers"],
                "bytes_per_token": bytes_per_token,
            }
            for quantity, kind in RESPONSES[direction].items():
                if direction == "file":
                    per_page = quantity in FILE_PER_PAGE
                    durations = row[quantity] if per_page else [row[quantity]]
                    if len(durations) != (pages if per_page else 1):
                        raise ValueError(f"File response has the wrong page count: {path}, line {position}")
                    for index, duration in enumerate(durations):
                        if type(duration) is not int or duration < 0:
                            raise ValueError(f"Invalid ns response: {path}, line {position}, {quantity}")
                        features = {"pages": 1 if per_page else pages, "page_bytes": tokens * bytes_per_token}
                        yield Measurement(position, f"{quantity}:{index}", *kind, features, duration, row["warmup"])
                    continue
                duration = row[quantity]
                if type(duration) is not int or duration < 0:
                    raise ValueError(f"Invalid ns response: {path}, line {position}, {quantity}")
                if quantity == "host_index_clone_ns":
                    features = {"elements": pages * tokens, "element_bytes": 8, "tensor_rank": 1}
                elif quantity == "load_prepare_return_ns":
                    features = {**work, "segments": segments}
                else:
                    features = work
                yield Measurement(position, quantity, *kind, features, duration, row["warmup"])
    if len(seen) != len(cases) * len(directions) * repetitions or not seen:
        raise ValueError(f"Capture does not contain all declared warmup and measured cases: {path}")
