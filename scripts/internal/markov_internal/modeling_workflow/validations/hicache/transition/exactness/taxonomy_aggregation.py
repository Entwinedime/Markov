"""Aggregation of transition-family catalog entries."""

from __future__ import annotations

from typing import Any

from .taxonomy_reviews import review_for_transition_family


def aggregate_transition_families(entries: list[dict[str, Any]], *, sample_limit: int) -> dict[str, Any]:
    """Aggregate transition entries by family and attach mechanism reviews."""

    families: dict[str, Any] = {}
    for entry in entries:
        family = str(entry.get("family") or "unresolved_transition_mismatch")
        item = families.setdefault(
            family,
            {
                "family": family,
                "classification": entry.get("classification"),
                "status": entry.get("status"),
                "patch_risk": entry.get("patch_risk"),
                "prediction_count": 0,
                "exact_count": 0,
                "target_config_ids": [],
                "input_ids": [],
                "source_config_ids": [],
                "mismatch_kind_counts": {},
                "mismatch_totals_by_kind": {},
                "sample_pages": [],
                "sample_predictions": [],
                "mechanism_review": review_for_transition_family(family),
                "patch_filter_action": entry.get("patch_filter_action"),
                "source_attribution_required": entry.get("source_attribution_required"),
                "duration_required": entry.get("duration_required"),
                "evidence_required": entry.get("evidence_required"),
            },
        )
        update_family_item(item, entry, sample_limit)
    for item in families.values():
        item["target_config_ids"] = sorted(item["target_config_ids"])
        item["input_ids"] = sorted(item["input_ids"])
        item["source_config_ids"] = sorted(item["source_config_ids"])
        item["mismatch_kind_counts"] = dict(sorted(item["mismatch_kind_counts"].items()))
        item["mismatch_totals_by_kind"] = dict(sorted(item["mismatch_totals_by_kind"].items()))
    return dict(sorted(families.items()))


def update_family_item(item: dict[str, Any], entry: dict[str, Any], sample_limit: int) -> None:
    """Merge one prediction classification into a family aggregate."""

    item["prediction_count"] += 1
    item["exact_count"] += int(bool(entry.get("exact")))
    append_unique(item["target_config_ids"], entry.get("target_config_id"))
    append_unique(item["input_ids"], entry.get("input_id"))
    append_unique(item["source_config_ids"], entry.get("source_config_id"))
    for kind in entry.get("mismatch_kinds", []):
        increment_nested_count(item, "mismatch_kind_counts", str(kind), 1)
    merge_mismatch_totals(item["mismatch_totals_by_kind"], entry.get("mismatch_totals_by_kind", {}))
    for page in entry.get("sample_pages", []):
        append_unique(item["sample_pages"], page, limit=sample_limit)
    if len(item["sample_predictions"]) < sample_limit:
        item["sample_predictions"].append(
            {
                "label": entry.get("label"),
                "prediction_dir": entry.get("prediction_dir"),
                "transition_exactness_path": entry.get("transition_exactness_path"),
                "mismatch_kinds": entry.get("mismatch_kinds"),
                "classification_reason": entry.get("classification_reason"),
                "sample_mismatches": entry.get("sample_mismatches", [])[: min(5, sample_limit)],
            }
        )


def append_unique(values: list[Any], value: Any, *, limit: int | None = None) -> None:
    """Append one unique, non-empty value within an optional limit."""

    if value is None or value == "":
        return
    if value in values:
        return
    if limit is not None and len(values) >= limit:
        return
    values.append(value)


def increment_nested_count(item: dict[str, Any], key: str, value: str, amount: int) -> None:
    """Increment one nested counter-like field."""

    counts = item.setdefault(key, {})
    counts[value] = int(counts.get(value, 0)) + amount


def merge_mismatch_totals(target: dict[str, Any], source: Any) -> None:
    """Merge per-kind mismatch totals into an aggregate."""

    if not isinstance(source, dict):
        return
    for kind, value in source.items():
        if not isinstance(value, dict):
            continue
        item = target.setdefault(str(kind), {"mismatch_rows": 0, "missing_in_model": 0, "extra_in_model": 0})
        item["mismatch_rows"] += int(value.get("mismatch_rows") or 0)
        item["missing_in_model"] += int(value.get("missing_in_model") or 0)
        item["extra_in_model"] += int(value.get("extra_in_model") or 0)
