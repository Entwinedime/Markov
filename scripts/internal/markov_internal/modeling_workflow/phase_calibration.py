"""Interpretable Prefill/Decode model from base and fixed-calibration work."""

from __future__ import annotations

import itertools
import math
from statistics import median
from typing import Any


def prefill_features(new_tokens: int, context_tokens: int) -> tuple[float, float, float]:
    return 1.0, float(new_tokens), new_tokens * (context_tokens + new_tokens / 2.0)


def decode_features(context_tokens: int, page_size: int, kernel_page_tokens: int) -> tuple[float, float, float]:
    effective_pages = math.ceil(context_tokens / min(page_size, kernel_page_tokens))
    return 1.0, float(context_tokens), float(effective_pages)


def paged_attention_family(row: dict[str, Any]) -> dict[str, int]:
    families = row.get("decode_kernel_families")
    if not isinstance(families, dict):
        raise ValueError("Decode observations are missing kernel families")
    values = [value for name, value in families.items() if "attention" in name.lower()]
    if not values:
        raise ValueError("Decode observations are missing paged-attention work")
    return {key: sum(int(value[key]) for value in values) for key in ("duration_us", "node_count")}


def _solve(matrix: list[list[float]], vector: list[float]) -> list[float] | None:
    size = len(vector)
    values = [row[:] + [vector[index]] for index, row in enumerate(matrix)]
    scale = max((abs(value) for row in matrix for value in row), default=0.0)
    for column in range(size):
        pivot = max(range(column, size), key=lambda row: abs(values[row][column]))
        if abs(values[pivot][column]) <= max(scale, 1.0) * 1e-12:
            return None
        values[column], values[pivot] = values[pivot], values[column]
        divisor = values[column][column]
        values[column] = [value / divisor for value in values[column]]
        for row in range(size):
            if row == column:
                continue
            factor = values[row][column]
            values[row] = [left - factor * right for left, right in zip(values[row], values[column])]
    return [values[index][-1] for index in range(size)]


def _fit_nonnegative(rows: list[dict[str, float]], target: str, features: tuple[str, ...]) -> dict[str, float]:
    best: tuple[float, dict[str, float]] | None = None
    for count in range(1, len(features) + 1):
        for active in itertools.combinations(features, count):
            matrix = [[sum(row[left] * row[right] for row in rows) for right in active] for left in active]
            vector = [sum(row[name] * row[target] for row in rows) for name in active]
            solution = _solve(matrix, vector)
            if solution is None or any(value < 0 for value in solution):
                continue
            coefficients = dict(zip(active, solution))
            error = sum((sum(coefficients.get(name, 0.0) * row[name] for name in features) - row[target]) ** 2
                        for row in rows)
            if best is None or error < best[0]:
                best = error, coefficients
    if best is None:
        raise ValueError(f"phase observations do not identify a non-negative {target} model")
    return {name: best[1].get(name, 0.0) for name in features}


def _metrics(actual: list[float], predicted: list[float]) -> dict[str, float]:
    errors = sorted(abs(value - estimate) / max(value, 1.0) for value, estimate in zip(actual, predicted))
    return {
        "wape": sum(abs(value - estimate) for value, estimate in zip(actual, predicted)) / sum(actual),
        "p90_ape": errors[min(len(errors) - 1, math.ceil(0.9 * len(errors)) - 1)],
    }


def _rows(captures: list[dict[str, Any]]) -> list[dict[str, Any]]:
    rows = []
    for capture in captures:
        observed = capture.get("source_phase_observations", {})
        if observed.get("status") != "ready":
            raise ValueError("phase observations are not ready")
        for row in observed["observations"]:
            request_ids = row.get("request_ids")
            if not isinstance(request_ids, list) or len(request_ids) != 1:
                raise ValueError("phase calibration requires one request per observed batch")
            prompt = int(row["prompt_token_count"])
            new = int(row["prefill_token_count"])
            if new <= 0 or new > prompt:
                raise ValueError("phase calibration found invalid token work")
            rows.append({
                "source_manifest": capture["source_manifest"],
                "source_manifests": [capture["source_manifest"]],
                "role": capture["role"],
                "request": str(request_ids[0]),
                "rank": int(row["logical_input"]),
                "page_size": int(row["source_page_size"]),
                "prompt": prompt,
                "new": new,
                "context": prompt - new,
                "attention": new * (prompt - new + new / 2.0),
                "common": float(row["prefill_common_kernel_duration_us"]),
                "prefix": float(row["prefill_prefix_attention_duration_us"]),
                "prefill_collective": float(row["prefill_collective_duration_us"]),
                "decode_iterations": int(row["decode_iteration_count"]),
                "decode_collective": float(row["decode_collective_duration_us"]),
                "paged": float(paged_attention_family(row)["duration_us"]),
            })
    return rows


_DURATION_FIELDS = (
    "common",
    "prefix",
    "prefill_collective",
    "decode_collective",
    "paged",
)


def _collapse_calibration_repeats(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Make repeats at each fixed endpoint one logical phase experiment."""

    bases = [row for row in rows if row["role"] == "base"]
    calibration = [row for row in rows if row["role"] == "calibration"]
    if not calibration:
        return bases
    manifests_by_page = {
        page: {row["source_manifest"] for row in calibration if row["page_size"] == page}
        for page in {row["page_size"] for row in calibration}
    }
    grouped: dict[tuple[Any, ...], list[dict[str, Any]]] = {}
    for row in calibration:
        key = (
            row["request"],
            row["rank"],
            row["page_size"],
            row["prompt"],
            row["new"],
            row["context"],
            row["decode_iterations"],
        )
        grouped.setdefault(key, []).append(row)
    if any({row["source_manifest"] for row in values} != manifests_by_page[values[0]["page_size"]]
           for values in grouped.values()):
        raise ValueError("fixed calibration repeats do not contain the same phase observations")

    collapsed = []
    for values in grouped.values():
        row = dict(values[0])
        row.update(
            source_manifest=f"fixed_calibration/page_{row['page_size']}",
            source_manifests=sorted(manifests_by_page[row["page_size"]]),
            repeat_count=len(values),
        )
        for field in _DURATION_FIELDS:
            row[field] = median(value[field] for value in values)
        collapsed.append(row)
    return [*bases, *collapsed]


def _intrinsic_requests(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    grouped: dict[tuple[str, str], list[dict[str, Any]]] = {}
    for row in rows:
        grouped.setdefault((row["source_manifest"], row["request"]), []).append(row)
    result = []
    for key, values in grouped.items():
        if len({row["rank"] for row in values}) != len(values) or len(values) < 2:
            raise ValueError(f"phase collective requires distinct ranks for {key[1]}")
        row = dict(values[0])
        row["prefill_collective"] = min(value["prefill_collective"] for value in values)
        row["decode_collective"] = min(value["decode_collective"] for value in values)
        row["paged"] = min(value["paged"] for value in values)
        result.append(row)
    return result


def _token_curve(rows: list[dict[str, Any]], target: str) -> tuple[list[dict[str, float]], dict[str, Any]]:
    """Build one measured token-cost curve with equal logical-source weight."""

    grouped: dict[tuple[str, int], list[dict[str, Any]]] = {}
    for row in rows:
        grouped.setdefault((row["source_manifest"], row["new"]), []).append(row)
    anchors = [
        {
            "role": values[0]["role"],
            "source_manifest": source,
            "new_tokens": new_tokens,
            "sample_count": len(values),
            "duration_us": median(row[target] for row in values),
        }
        for (source, new_tokens), values in sorted(grouped.items())
    ]
    raw_points = [
        {
            "new_tokens": new_tokens,
            "duration_us": median(row["duration_us"] for row in anchors if row["new_tokens"] == new_tokens),
        }
        for new_tokens in sorted({row["new_tokens"] for row in anchors})
    ]
    if len(raw_points) < 2:
        raise ValueError(f"phase observations do not identify a {target} token curve")
    points = []
    for raw in raw_points:
        points.append({
            **raw,
            # More token work cannot reduce intrinsic cost.  Clamp only the
            # downward measurement noise; do not fit another free coefficient.
            "duration_us": max(raw["duration_us"], points[-1]["duration_us"] if points else 0.0),
        })
    adjustment = sum(point["duration_us"] - raw["duration_us"] for raw, point in zip(raw_points, points))
    return points, {
        "formula": (
            "median duration at each observed new-token anchor; clamp downward measurement noise to the previous "
            "anchor; linear interpolation between anchors"
        ),
        "unit": "microseconds",
        "logical_anchors": anchors,
        "raw_points": raw_points,
        "points": points,
        "monotonic_adjustment_us": adjustment,
    }


def _curve_value(points: list[dict[str, float]], new_tokens: int) -> float:
    if new_tokens <= points[0]["new_tokens"]:
        left, right = points[:2]
    elif new_tokens >= points[-1]["new_tokens"]:
        left, right = points[-2:]
    else:
        left, right = next(
            (left, right) for left, right in zip(points, points[1:]) if new_tokens <= right["new_tokens"]
        )
    position = (new_tokens - left["new_tokens"]) / (right["new_tokens"] - left["new_tokens"])
    return max(0.0, left["duration_us"] + position * (right["duration_us"] - left["duration_us"]))


def _component(coefficients: dict[str, float]) -> dict[str, float]:
    return {
        "fixed_us": coefficients.get("fixed", 0.0),
        "per_new_token_us": coefficients.get("new", 0.0),
        "per_attention_token_pair_us": coefficients.get("attention", 0.0),
        "per_context_token_us": coefficients.get("context", 0.0),
    }


def build_phase_cost(captures: list[dict[str, Any]], base_page_size: int) -> tuple[dict[str, Any], dict[str, Any]]:
    raw_rows = _rows(captures)
    rows = _collapse_calibration_repeats(raw_rows)
    intrinsic = _intrinsic_requests(rows)
    common_points, common_source = _token_curve(rows, "common")
    collective_points, collective_source = _token_curve(intrinsic, "prefill_collective")

    prefix_rows = []
    for row in rows:
        if row["prefix"] <= 0:
            continue
        fixed, new, attention = prefill_features(row["new"], row["context"])
        prefix_rows.append({"fixed": fixed, "new": new, "attention": attention, "duration": row["prefix"]})
    prefix_coefficients = _fit_nonnegative(prefix_rows, "duration", ("fixed", "new", "attention"))

    decode_rows = []
    for row in rows:
        if row["decode_iterations"] <= 0 or row["paged"] <= 0:
            continue
        fixed, context, pages = decode_features(row["prompt"], row["page_size"], base_page_size)
        decode_rows.append({"fixed": fixed, "context": context, "pages": pages,
                            "duration": row["paged"] / row["decode_iterations"]})
    decode_coefficients = _fit_nonnegative(decode_rows, "duration", ("fixed", "context", "pages"))
    collective_per_iteration = [row["decode_collective"] / row["decode_iterations"] for row in intrinsic
                                if row["decode_iterations"] > 0 and row["decode_collective"] > 0]
    if not collective_per_iteration:
        raise ValueError("Decode collective work was not observed")

    phase = {
        "prefill_common_kernel": {"new_token_points": common_points},
        "prefill_collective": {"new_token_points": collective_points},
        "prefill_prefix_attention": _component(prefix_coefficients),
        "decode_paged_attention": {
            "kernel_page_tokens": base_page_size,
            "fixed_us_per_iteration": decode_coefficients["fixed"],
            "per_context_token_us": decode_coefficients["context"],
            "per_effective_page_us": decode_coefficients["pages"],
        },
        "decode_collective": _component({"fixed": median(collective_per_iteration)}),
        "coverage": {
            "min_new_tokens": min(row["new"] for row in rows),
            "max_new_tokens": max(row["new"] for row in rows),
            "min_context_tokens": min(row["context"] for row in rows),
            "max_context_tokens": max(row["context"] for row in rows),
            "min_attention_token_pairs": min(row["attention"] for row in rows),
            "max_attention_token_pairs": max(row["attention"] for row in rows),
            "min_decode_context_tokens": min(row["prompt"] for row in rows if row["decode_iterations"] > 0),
            "max_decode_context_tokens": max(row["prompt"] for row in rows if row["decode_iterations"] > 0),
            "base_page_size": base_page_size,
        },
    }
    common_actual = [row["common"] for row in rows]
    common_predicted = [_curve_value(common_points, row["new"]) for row in rows]
    collective_actual = [row["prefill_collective"] for row in intrinsic]
    collective_predicted = [_curve_value(collective_points, row["new"]) for row in intrinsic]
    prefix_actual = [row["duration"] for row in prefix_rows]
    prefix_predicted = [sum(prefix_coefficients[name] * row[name] for name in prefix_coefficients) for row in prefix_rows]
    decode_actual = [row["duration"] for row in decode_rows]
    decode_predicted = [sum(decode_coefficients[name] * row[name] for name in decode_coefficients) for row in decode_rows]
    sources = [{"source_manifest": capture["source_manifest"], "role": capture["role"]} for capture in captures]
    summary = {
        "status": "ready",
        "source_manifests": sources,
        "target_inputs": [],
        "parameter_sources": {
            "common_kernel": common_source,
            "prefix_attention": "non-negative fixed + new_tokens + attention_token_pairs",
            "prefill_collective": collective_source,
            "decode_paged_attention": "non-negative fixed + context_tokens + effective_pages; kernel page is base page",
            "decode_collective": "median rank-min duration per decode iteration",
        },
        "fit": {
            "prefill_common_kernel": _metrics(common_actual, common_predicted),
            "prefill_prefix_attention": _metrics(prefix_actual, prefix_predicted),
            "prefill_collective": _metrics(collective_actual, collective_predicted),
            "decode_paged_attention": _metrics(decode_actual, decode_predicted),
        },
        "raw_observation_count": len(raw_rows),
        "logical_observation_count": len(rows),
        "request_count": len(intrinsic),
        "fixed_calibration_repeat_count_by_endpoint": {
            str(page): len({row["source_manifest"] for row in raw_rows
                            if row["role"] == "calibration" and row["page_size"] == page})
            for page in sorted({row["page_size"] for row in raw_rows if row["role"] == "calibration"})
        },
        "repeat_weighting": "identical repeats form one median logical anchor per page endpoint",
    }
    return phase, summary
