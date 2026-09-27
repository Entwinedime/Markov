"""Interpretable Prefill/Decode model from base and fixed-calibration work."""

from __future__ import annotations

import itertools
import math
from statistics import median
from typing import Any


class MissingPhaseEvidence(ValueError):
    """Required measured work is absent; malformed input is a separate error."""


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


def _feature_matrix(
    rows: list[dict[str, float]], features: tuple[str, ...]
) -> tuple[dict[str, float], dict[str, dict[str, float]]]:
    """Build normalized feature products once for fitting and identifiability."""

    # Token pairs can be millions while the fixed column is one. Normalize
    # units before solving so large features do not erase the fixed term.
    scales = {name: max((abs(row[name]) for row in rows), default=0.0) or 1.0 for name in features}
    columns = {name: [row[name] / scales[name] for row in rows] for name in features}
    matrix = {
        left: {right: sum(a * b for a, b in zip(columns[left], columns[right])) for right in features}
        for left in features
    }
    return scales, matrix


def _fit_nonnegative(rows: list[dict[str, float]], target: str, features: tuple[str, ...]) -> dict[str, float]:
    scales, products = _feature_matrix(rows, features)
    responses = {name: sum((row[name] / scales[name]) * row[target] for row in rows) for name in features}
    best: tuple[float, dict[str, float]] | None = None
    for count in range(1, len(features) + 1):
        for active in itertools.combinations(features, count):
            matrix = [[products[left][right] for right in active] for left in active]
            vector = [responses[name] for name in active]
            solution = _solve(matrix, vector)
            if solution is None or any(value < 0 for value in solution):
                continue
            coefficients = {name: value / scales[name] for name, value in zip(active, solution)}
            error = sum(
                (sum(coefficients.get(name, 0.0) * row[name] for name in features) - row[target]) ** 2 for row in rows
            )
            if best is None or error < best[0]:
                best = error, coefficients
    if best is None:
        raise MissingPhaseEvidence(f"phase observations do not identify a non-negative {target} model")
    return {name: best[1].get(name, 0.0) for name in features}


def _feature_rank(rows: list[dict], features: tuple[str, ...]) -> int:
    _, products = _feature_matrix(rows, features)
    for count in range(len(features), 0, -1):
        for active in itertools.combinations(features, count):
            matrix = [[products[left][right] for right in active] for left in active]
            if _solve(matrix, [0.0] * count) is not None:
                return count
    return 0


def _regression_evidence(rows: list[dict], features: tuple[str, ...]) -> tuple[list[dict], dict]:
    """Add shared captures only when they distinguish another feature direction.

    Selection uses measured work, never fitted error or target timings. Rank is
    an identifiability check, not a guarantee of conditioning or extrapolation accuracy.
    """
    selected = [row for row in rows if row["role"] == "base"]
    rank = base_rank = _feature_rank(selected, features)
    candidates: dict[str, list[dict]] = {}
    for row in rows:
        if row["role"] == "calibration":
            candidates.setdefault(row["source_manifest"], []).append(row)
    while candidates and rank < len(features):
        choices = [
            (_feature_rank(selected + values, features), -len(values), source) for source, values in candidates.items()
        ]
        new_rank, _, source = max(choices)
        if new_rank <= rank:
            break
        selected.extend(candidates.pop(source))
        rank = new_rank
    if not selected:
        raise MissingPhaseEvidence("no measured work for " + ", ".join(features))
    return selected, dict(
        features=list(features),
        base_feature_rank=base_rank,
        selected_feature_rank=rank,
        feature_ranges={
            name: [min(row[name] for row in selected), max(row[name] for row in selected)] for name in features
        },
        evidence_origin="base"
        if all(row["role"] == "base" for row in selected)
        else "base_plus_independent"
        if base_rank
        else "independent_calibration",
        source_manifests=sorted({source for row in selected for source in row["source_manifests"]}),
        limitation=(
            "coefficients are not separately identifiable; prediction outside measured feature relations is uncertain"
            if rank < len(features)
            else "full feature rank does not verify accuracy outside measured work"
        ),
    )


def _rows(captures: list[dict[str, Any]]) -> list[dict[str, Any]]:
    rows = []
    for capture in captures:
        observed = capture.get("source_phase_observations", {})
        if observed.get("status") != "ready":
            raise MissingPhaseEvidence("phase observations are not ready")
        for row in observed["observations"]:
            request_ids = row.get("request_ids")
            if not isinstance(request_ids, list) or len(request_ids) != 1:
                raise ValueError("phase calibration requires one request per observed batch")
            prompt = int(row["prompt_token_count"])
            new = int(row["prefill_token_count"])
            if new <= 0 or new > prompt:
                raise ValueError("phase calibration found invalid token work")
            rows.append(
                {
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
                    "paged": float(paged_attention_family(row)["duration_us"])
                    if row["decode_iteration_count"]
                    else 0.0,
                }
            )
    return rows


_DURATION_FIELDS = (
    "common",
    "prefix",
    "prefill_collective",
    "decode_collective",
    "paged",
)


def _collapse_calibration_repeats(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Collapse identical measured work, not every experiment at the same page size."""

    bases = [row for row in rows if row["role"] == "base"]
    by_source: dict[str, list[dict[str, Any]]] = {}
    for row in rows:
        if row["role"] == "calibration":
            by_source.setdefault(row["source_manifest"], []).append(row)
    work_fields = ("request", "rank", "page_size", "prompt", "new", "context", "decode_iterations")
    cohorts: dict[tuple, list[list[dict[str, Any]]]] = {}
    for values in by_source.values():
        ordered = sorted(values, key=lambda row: tuple(row[field] for field in work_fields))
        work = tuple(tuple(row[field] for field in work_fields) for row in ordered)
        cohorts.setdefault(work, []).append(ordered)
    for repeats in cohorts.values():
        manifests = sorted(values[0]["source_manifest"] for values in repeats)
        for values in zip(*repeats):
            row = dict(values[0], source_manifest=manifests[0], source_manifests=manifests, repeat_count=len(repeats))
            row.update({field: median(value[field] for value in values) for field in _DURATION_FIELDS})
            bases.append(row)
    return bases


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
    """Use a base curve when identifiable; supplement only missing token anchors."""

    bases = [row for row in rows if row["role"] == "base"]
    base_tokens = {row["new"] for row in bases}
    rows = (
        bases
        if len(base_tokens) >= 2
        else [*bases, *(row for row in rows if row["role"] == "calibration" and row["new"] not in base_tokens)]
    )
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
        raise MissingPhaseEvidence(
            f"phase observations do not identify a {target} token curve: two token anchors required"
        )
    points = []
    for raw in raw_points:
        points.append(
            {
                **raw,
                # More token work cannot reduce intrinsic cost.  Clamp only the
                # downward measurement noise; do not fit another free coefficient.
                "duration_us": max(raw["duration_us"], points[-1]["duration_us"] if points else 0.0),
            }
        )
    adjustment = sum(point["duration_us"] - raw["duration_us"] for raw, point in zip(raw_points, points))
    return points, {
        "formula": (
            "median duration at each observed new-token anchor; clamp downward measurement noise to the previous "
            "anchor; linear interpolation between anchors"
        ),
        "unit": "microseconds",
        "evidence_origin": "base"
        if len(base_tokens) >= 2
        else "base_plus_independent"
        if bases
        else "independent_calibration",
        "extrapolation": "endpoint linear slope, clamped at zero; outside measured token range is unverified",
        "logical_anchors": anchors,
        "raw_points": raw_points,
        "points": points,
        "monotonic_adjustment_us": adjustment,
    }


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
        prefix_rows.append({**row, "fixed": fixed, "new": new, "attention": attention, "duration": row["prefix"]})
    prefix_rows, prefix_source = _regression_evidence(prefix_rows, ("fixed", "new", "attention"))
    prefix_coefficients = _fit_nonnegative(prefix_rows, "duration", ("fixed", "new", "attention"))

    needs_decode = any(row["role"] == "base" and row["decode_iterations"] > 0 for row in rows)
    if needs_decode:
        decode_rows = []
        for row in rows:
            if row["decode_iterations"] <= 0 or row["paged"] <= 0:
                continue
            fixed, context, pages = decode_features(row["prompt"], row["page_size"], base_page_size)
            decode_rows.append(
                {
                    **row,
                    "fixed": fixed,
                    "context": context,
                    "pages": pages,
                    "duration": row["paged"] / row["decode_iterations"],
                }
            )
        decode_rows, decode_source = _regression_evidence(decode_rows, ("fixed", "context", "pages"))
        decode_coefficients = _fit_nonnegative(decode_rows, "duration", ("fixed", "context", "pages"))
        collective_rows = [row for row in intrinsic if row["decode_iterations"] > 0 and row["decode_collective"] > 0]
        collective_rows = [row for row in collective_rows if row["role"] == "base"] or collective_rows
        collective_per_iteration = [row["decode_collective"] / row["decode_iterations"] for row in collective_rows]
        if not collective_per_iteration:
            raise MissingPhaseEvidence("Decode collective work was not observed")

    phase = {
        "prefill_common_kernel": {"new_token_points": common_points},
        "prefill_collective": {"new_token_points": collective_points},
        "prefill_prefix_attention": _component(prefix_coefficients),
        "decode_paged_attention": {
            "kernel_page_tokens": base_page_size,
            "fixed_us_per_iteration": decode_coefficients["fixed"],
            "per_context_token_us": decode_coefficients["context"],
            "per_effective_page_us": decode_coefficients["pages"],
        }
        if needs_decode
        else None,
        "decode_collective": _component({"fixed": median(collective_per_iteration)}) if needs_decode else None,
        "coverage": {
            "min_new_tokens": min(row["new"] for row in rows),
            "max_new_tokens": max(row["new"] for row in rows),
            "min_context_tokens": min(row["context"] for row in rows),
            "max_context_tokens": max(row["context"] for row in rows),
            "min_attention_token_pairs": min(row["attention"] for row in rows),
            "max_attention_token_pairs": max(row["attention"] for row in rows),
            "min_decode_context_tokens": min(row["prompt"] for row in rows if row["decode_iterations"] > 0)
            if needs_decode
            else None,
            "max_decode_context_tokens": max(row["prompt"] for row in rows if row["decode_iterations"] > 0)
            if needs_decode
            else None,
            "base_page_size": base_page_size,
        },
    }
    summary = {
        "status": "ready",
        "parameter_sources": {
            "common_kernel": common_source,
            "prefix_attention": prefix_source,
            "prefill_collective": collective_source,
            "decode_paged_attention": decode_source
            if needs_decode
            else {"reason": "base_workloads_have_no_decode_iterations"},
            "decode_collective": dict(
                formula="median rank-min duration per decode iteration",
                evidence_origin="base" if collective_rows[0]["role"] == "base" else "independent_calibration",
                source_manifests=sorted({source for row in collective_rows for source in row["source_manifests"]}),
                limitation="assumes constant collective cost per iteration within the same TP and runtime",
            )
            if needs_decode
            else {"reason": "base_workloads_have_no_decode_iterations"},
        },
        "raw_observation_count": len(raw_rows),
        "logical_observation_count": len(rows),
        "request_count": len(intrinsic),
        "repeat_weighting": "captures with identical request/rank/token/page work form one median logical experiment",
    }
    return phase, summary
