#!/usr/bin/env python3
"""Summarize HiCache model/oracle provenance for mismatched pages.

This is a diagnostic helper. It does not feed oracle data back into the model.
It reads validation diffs, predicted transition traces, and oracle state
snapshots, then reports the per-page evidence needed to decide whether a
backend rule is fixable from current invariant facts or requires new profiling.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from model_runner import (  # noqa: E402
    derived_hicache_state_from_snapshot,
    extract_hicache_state_snapshots,
    latest_derived_state,
    load_json,
    normalize_hicache_state_for_oracle_compare,
    snapshot_timeline_sort_key,
)


ACTIVE_STATE_KEYS = (
    "l1_resident_pages",
    "l2_resident_pages",
    "dirty_pages",
    "backuped_pages",
    "evicted_pages",
    "locked_pages",
)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--validation", type=Path, required=True)
    parser.add_argument("--predicted-trace", type=Path, required=True)
    parser.add_argument("--oracle-trace", type=Path, action="append", default=[])
    parser.add_argument("--page", action="append", default=[], help="Normalized or scoped page key to include explicitly.")
    parser.add_argument("--sample-per-set", type=int, default=3)
    parser.add_argument("--max-model-transitions", type=int, default=24)
    parser.add_argument("--max-oracle-changes", type=int, default=24)
    parser.add_argument("--output", type=Path)
    return parser.parse_args(argv)


def normalize_page_key(page: Any) -> str:
    text = str(page)
    if "|" in text:
        return text.rsplit("|", 1)[-1]
    return text


def normalized_memberships(state: dict[str, Any], page: str) -> list[str]:
    memberships: list[str] = []
    for key in ACTIVE_STATE_KEYS:
        values = state.get(key)
        if not isinstance(values, list):
            continue
        if page in {normalize_page_key(item) for item in values}:
            memberships.append(key)
    return memberships


def collect_pages(validation: dict[str, Any], explicit_pages: list[str], sample_per_set: int) -> dict[str, dict[str, Any]]:
    selected: dict[str, dict[str, Any]] = {}

    def add(page: Any, reason: str, tier: str | None = None, side: str | None = None) -> None:
        normalized = normalize_page_key(page)
        if not normalized:
            return
        entry = selected.setdefault(normalized, {"page": normalized, "reasons": []})
        entry["reasons"].append({"reason": reason, "tier": tier, "side": side})

    for page in explicit_pages:
        add(page, "explicit")

    hicache = validation.get("hicache_state") if isinstance(validation.get("hicache_state"), dict) else {}
    first = hicache.get("first_mismatch")
    if isinstance(first, dict):
        add(first.get("page"), "first_mismatch", first.get("tier"), "missing_in_model")

    diffs = hicache.get("sets_diff_by_tier")
    if isinstance(diffs, dict):
        for tier, diff in diffs.items():
            if not isinstance(diff, dict):
                continue
            for page in list(diff.get("missing_in_model") or [])[:sample_per_set]:
                add(page, "sets_diff_sample", str(tier), "missing_in_model")
            for page in list(diff.get("extra_in_model") or [])[:sample_per_set]:
                add(page, "sets_diff_sample", str(tier), "extra_in_model")
    return selected


def summarize_model_trace(predicted_trace: dict[str, Any], pages: set[str], max_transitions: int) -> tuple[dict[str, Any], dict[str, Any]]:
    records = predicted_trace.get("records")
    if not isinstance(records, list):
        records = []
    by_page: dict[str, list[dict[str, Any]]] = {page: [] for page in pages}
    counts_by_page_kind: dict[str, dict[str, int]] = {page: {} for page in pages}
    for record in records:
        if not isinstance(record, dict):
            continue
        record_pages = record.get("target_page_set")
        if not isinstance(record_pages, list):
            continue
        touched = {normalize_page_key(item) for item in record_pages}
        for page in pages.intersection(touched):
            kind = str(record.get("transition_kind") or "")
            counts_by_page_kind[page][kind] = counts_by_page_kind[page].get(kind, 0) + 1
            if len(by_page[page]) >= max_transitions:
                continue
            by_page[page].append(
                {
                    "transition_kind": record.get("transition_kind"),
                    "event_base_name": record.get("event_base_name"),
                    "source_event_name": record.get("source_event_name"),
                    "request_id": record.get("request_id"),
                    "operation_id": record.get("operation_id"),
                    "cache_scope": record.get("cache_scope"),
                    "ts": record.get("ts"),
                    "source_event_index": record.get("source_event_index"),
                    "tier_src": record.get("tier_src"),
                    "tier_dst": record.get("tier_dst"),
                }
            )

    model_final = predicted_trace.get("final_state")
    if not isinstance(model_final, dict):
        model_final = {}
    normalized_model_final = normalize_hicache_state_for_oracle_compare(model_final, "strip_scope")
    final_memberships = {page: normalized_memberships(normalized_model_final, page) for page in pages}
    return (
        {
            page: {
                "final_memberships": final_memberships.get(page, []),
                "transition_counts": counts_by_page_kind.get(page, {}),
                "sampled_transitions": by_page.get(page, []),
            }
            for page in sorted(pages)
        },
        normalized_model_final,
    )


def union_normalized_memberships(object_states: dict[tuple[str, str, str], dict[str, Any]], pages: set[str]) -> dict[str, list[str]]:
    result = {page: [] for page in pages}
    for state in object_states.values():
        normalized = normalize_hicache_state_for_oracle_compare(state, "strip_scope")
        for page in pages:
            memberships = set(result[page])
            memberships.update(normalized_memberships(normalized, page))
            result[page] = sorted(memberships)
    return result


def summarize_oracle_trace(trace_paths: list[Path], pages: set[str], max_changes: int) -> tuple[dict[str, Any], dict[str, Any]]:
    snapshots = extract_hicache_state_snapshots(trace_paths)
    oracle_final = normalize_hicache_state_for_oracle_compare(latest_derived_state(snapshots), "strip_scope")
    final_memberships = {page: normalized_memberships(oracle_final, page) for page in pages}

    object_states: dict[tuple[str, str, str], dict[str, Any]] = {}
    last_memberships = {page: [] for page in pages}
    changes: dict[str, list[dict[str, Any]]] = {page: [] for page in pages}
    for row in sorted(snapshots, key=snapshot_timeline_sort_key):
        snapshot = row.get("state_snapshot")
        if not isinstance(snapshot, dict) or not snapshot.get("enabled", False):
            continue
        object_type = str(row.get("object_type") or snapshot.get("object_type") or "")
        if "RadixCache" not in object_type:
            continue
        object_id = str(row.get("object_id") or snapshot.get("object_id") or "")
        if not object_id:
            continue
        key = (str(row.get("trace_path") or ""), str(row.get("pid") or ""), object_id)
        object_states[key] = derived_hicache_state_from_snapshot(snapshot)
        current = union_normalized_memberships(object_states, pages)
        for page in pages:
            if current[page] == last_memberships[page]:
                continue
            if len(changes[page]) < max_changes:
                changes[page].append(
                    {
                        "ts": row.get("ts"),
                        "event_name": row.get("event_name"),
                        "source_event_name": row.get("source_event_name"),
                        "target_id": row.get("target_id"),
                        "request_id": row.get("request_id"),
                        "operation_id": row.get("operation_id"),
                        "object_id": object_id,
                        "memberships": current[page],
                    }
                )
            last_memberships[page] = current[page]

    return (
        {
            page: {
                "final_memberships": final_memberships.get(page, []),
                "sampled_membership_changes": changes.get(page, []),
            }
            for page in sorted(pages)
        },
        oracle_final,
    )


def classify_fixability(model: dict[str, Any], oracle: dict[str, Any]) -> str:
    model_memberships = set(model.get("final_memberships") or [])
    oracle_memberships = set(oracle.get("final_memberships") or [])
    transition_counts = model.get("transition_counts") if isinstance(model.get("transition_counts"), dict) else {}
    if "locked_pages" in oracle_memberships and "locked_pages" not in model_memberships and transition_counts.get("mark_locked", 0) == 0:
        return "needs_lock_ref_provenance_or_collection"
    if "l1_resident_pages" in oracle_memberships and "evicted_pages" in model_memberships and transition_counts.get("mark_evicted", 0) > 0:
        return "likely_fixable_capacity_or_promotion_rule"
    if "l2_resident_pages" in oracle_memberships and "l2_resident_pages" not in model_memberships:
        return "likely_fixable_l2_backup_or_prefetch_visibility_rule"
    if "dirty_pages" in oracle_memberships and "dirty_pages" not in model_memberships:
        return "hard_without_writeback_dirty_order_evidence"
    return "needs_manual_trace_review"


def build_report(args: argparse.Namespace) -> dict[str, Any]:
    validation = load_json(args.validation)
    predicted_trace = load_json(args.predicted_trace)
    selected = collect_pages(validation, args.page, args.sample_per_set)
    pages = set(selected)
    model_by_page, model_final = summarize_model_trace(predicted_trace, pages, args.max_model_transitions)
    oracle_by_page, oracle_final = summarize_oracle_trace(args.oracle_trace, pages, args.max_oracle_changes) if args.oracle_trace else ({}, {})

    pages_report: dict[str, Any] = {}
    for page in sorted(pages):
        model = model_by_page.get(page, {})
        oracle = oracle_by_page.get(page, {})
        pages_report[page] = {
            "reasons": selected[page].get("reasons", []),
            "model": model,
            "oracle": oracle,
            "fixability_hint": classify_fixability(model, oracle) if oracle else "no_oracle_trace",
        }

    return {
        "validation": str(args.validation),
        "predicted_trace": str(args.predicted_trace),
        "oracle_traces": [str(path) for path in args.oracle_trace],
        "page_count": len(pages),
        "pages": pages_report,
        "final_counts": {
            "model": {key: len(value) for key, value in model_final.items() if isinstance(value, list)},
            "oracle": {key: len(value) for key, value in oracle_final.items() if isinstance(value, list)},
        },
    }


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    report = build_report(args)
    text = json.dumps(report, ensure_ascii=False, indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text + "\n", encoding="utf-8")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
