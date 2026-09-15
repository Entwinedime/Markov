"""Small fixed-cost models measured by the fixed endpoint calibration."""

from __future__ import annotations

from statistics import median
from typing import Any


def _logical_estimate(captures: list[dict[str, Any]], field: str, samples) -> tuple[float, dict[str, Any]]:
    """Collapse repeats per endpoint, then give each endpoint one vote."""

    per_capture = []
    for capture in captures:
        values = samples(capture)
        if values:
            pages = {int(row.get("page_size") or 0)
                     for row in capture["source_io_observations"]["observations"]
                     if int(row.get("page_size") or 0) > 0}
            if len(pages) != 1:
                raise ValueError("fixed calibration control profile must identify one page endpoint")
            per_capture.append({
                "role": capture["role"],
                "page_size": pages.pop(),
                "source_manifest": capture["source_manifest"],
                "sample_count": len(values),
                field: median(values),
            })
    anchors = []
    for page in sorted({row["page_size"] for row in per_capture}):
        fixed = [row for row in per_capture if row["page_size"] == page]
        anchors.append({
            "role": "fixed_calibration_endpoint",
            "page_size": page,
            "source_manifests": [row["source_manifest"] for row in fixed],
            "repeat_count": len(fixed),
            "sample_count": sum(row["sample_count"] for row in fixed),
            field: median(row[field] for row in fixed),
        })
    if not anchors:
        raise ValueError(f"base plus fixed calibration did not measure {field}")
    return median(row[field] for row in anchors), {
        "logical_anchor_count": len(anchors),
        "raw_sample_count": sum(row["sample_count"] for row in per_capture),
        "per_capture": per_capture,
        "logical_anchors": anchors,
        "estimator": "median of endpoint medians; repeated captures have no extra weight",
    }


def _prefetch_samples(capture: dict[str, Any]) -> tuple[list[float], list[float]]:
    commits = []
    checks = []
    for row in capture["source_io_observations"]["observations"]:
        if row.get("kind") != "prefetch" or not row.get("terminal_control_observed"):
            continue
        local_samples = [float(value) for value in row.get("progress_check_cpu_samples_us", []) if value > 0]
        if not local_samples:
            continue
        check = median(local_samples)
        commit = float(row["terminal_explicit_cpu_us"]) - check
        if commit > 0:
            commits.append(commit)
            checks.append(check)
    return commits, checks


def prefetch_control(captures: list[dict[str, Any]]) -> dict[str, Any]:
    """Separate one terminal commit from one policy state-check primitive."""

    by_manifest = {capture["source_manifest"]: _prefetch_samples(capture) for capture in captures}
    commit, commit_source = _logical_estimate(captures, "commit_us_per_operation", lambda capture: by_manifest[capture["source_manifest"]][0])
    check, check_source = _logical_estimate(captures, "state_check_us_per_operation", lambda capture: by_manifest[capture["source_manifest"]][1])
    return {
        "commit_us_per_operation": commit,
        "state_check_us_per_operation": check,
        "commit_source": commit_source,
        "state_check_source": check_source,
    }


def load_control(captures: list[dict[str, Any]]) -> dict[str, Any]:
    """One admission cost; eviction shape remains structure, not a fitted feature."""

    def samples(capture: dict[str, Any]) -> list[float]:
        values = []
        for row in capture["source_io_observations"]["observations"]:
            if (row.get("kind") == "load" and row.get("completed_tokens", 0) > 0
                    and row.get("admission_control_observed") and row.get("admission_control_us", 0) > 0):
                values.append(float(row["admission_control_us"]))
        return values

    estimate, source = _logical_estimate(captures, "fixed_us_per_operation", samples)
    return {
        "fixed_us_per_operation": estimate,
        "source": source,
    }


def control_models(captures: list[dict[str, Any]]) -> tuple[dict[str, Any], dict[str, Any]]:
    prefetch = prefetch_control(captures)
    load = load_control(captures)
    models = {
        "prefetch": {
            "fixed_us_per_operation": prefetch["commit_us_per_operation"],
            "state_check_us_per_operation": prefetch["state_check_us_per_operation"],
        },
        "load": {"fixed_us_per_operation": load["fixed_us_per_operation"]},
        "write_device_to_host": {"fixed_us_per_operation": 0.0},
        "write_host_to_storage": {"fixed_us_per_operation": 0.0},
    }
    return models, {"prefetch": prefetch, "load": load}
