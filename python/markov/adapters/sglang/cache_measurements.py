"""Translate verified normal cache-controller samples into independent cost facts."""

import json
from pathlib import Path

from markov.artifacts.performance import Measurement
from markov.artifacts.request import positive_integer, read_json


IMPLEMENTATION = "sglang.HiCacheController.write:kernel_ascend.page_first_direct"


def recipes(captured):
    limits = [
        "Independent initialized idle single-device response; no competing I/O or CPU load. "
        "The declared deployment is supplied by the maintainer, not inferred from a capture's device index."
    ]
    limits.append(captured["limitations"])
    return {
        ("cache.backup.caller", IMPLEMENTATION, "caller_blocking_interval"): {
            "feature": "pages",
            "curve": "page_bytes",
            "extrapolation_features": ["pages"],
            "limitations": limits
            + [
                "Opaque CPU preparation/DMA overlap; page-count extrapolation assumes fixed setup and repeated per-page work."
            ],
        },
        ("cache.backup.completion", IMPLEMENTATION, "completion_observation_upper_interval"): {
            "feature": "pages",
            "curve": "page_bytes",
            "extrapolation_features": ["pages"],
            "limitations": limits
            + [
                "Includes completion-observation API; a prediction is not a guaranteed upper bound or bare device service. "
                "Queue occupancy uses one coarse controller lane; page-count extrapolation is an unmeasured assumption."
            ],
        },
        ("cache.index_clone", "aten.clone:cpu:int64", "caller_blocking_interval"): {
            "feature": "elements",
            "limitations": limits + ["One complete host API response, not separately measured CPU self service."],
        },
    }


def metadata(path: Path):
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


def context(captured: dict, deployment: dict):
    if (deployment["framework"], deployment["runtime"], deployment["dtype"]) != ("sglang", "ascend", captured["dtype"]):
        raise ValueError("Cache measurements require a matching SGLang/Ascend deployment and dtype")

    # Single-device responses depend on local work, not model identity or global TP.
    return {
        "platform": deployment["platform"],
        "framework": deployment["framework"],
        "runtime": deployment["runtime"],
        **{key: captured[key] for key in ("torch", "dtype", "backend", "host_layout")},
    }


def measurements(path: Path, captured: dict):
    geometry = captured["recipe"]["geometry"]
    for name in ("layers", "kv_heads", "head_dim"):
        positive_integer(geometry[name], name)
    bytes_per_token = 4 * geometry["layers"] * geometry["kv_heads"] * geometry["head_dim"]
    kinds = {
        "caller_return_ns": ("cache.backup.caller", IMPLEMENTATION, "caller_blocking_interval"),
        "completion_observed_ns": ("cache.backup.completion", IMPLEMENTATION, "completion_observation_upper_interval"),
        "host_index_clone_ns": ("cache.index_clone", "aten.clone:cpu:int64", "caller_blocking_interval"),
    }
    recipe = captured["recipe"]
    warmup = positive_integer(recipe["warmup"], "warmup")
    repetitions = warmup + positive_integer(recipe["repeats"], "repeats")
    cases = {
        (positive_integer(c["page_tokens"], "page_tokens"), positive_integer(c["pages"], "pages"))
        for c in recipe["cases"]
    }
    seen = set()
    with (path.parent / "measurements.jsonl").open(encoding="utf-8") as stream:
        for position, line in enumerate(stream, start=1):
            row = json.loads(line)
            pages = positive_integer(row["pages"], "pages")
            tokens = positive_integer(row["page_tokens"], "page_tokens")
            ordinal = row["ordinal"]
            identity = (tokens, pages, ordinal)
            if (
                (tokens, pages) not in cases
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
            for quantity, kind in kinds.items():
                duration = row[quantity]
                if type(duration) is not int or duration < 0:
                    raise ValueError(f"Invalid ns response: {path}, line {position}, {quantity}")
                features = (
                    {"elements": pages * tokens, "element_bytes": 8, "tensor_rank": 1}
                    if quantity == "host_index_clone_ns"
                    else work
                )
                yield Measurement(position, quantity, *kind, features, duration, row["warmup"])
    if len(seen) != len(cases) * repetitions or not seen:
        raise ValueError(f"Capture does not contain all declared warmup and measured cases: {path}")
