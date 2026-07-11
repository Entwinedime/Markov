"""Convert a C++ HiCache summary into the prediction trace used by validation."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from markov_internal.common.io import load_json, write_json

from ..oracle.snapshot.state import event_base_name


def write_predicted_state_trace(module_summary_path: Path, output_dir: Path) -> Path | None:
    """Write a prediction trace when a valid C++ HiCache summary exists."""

    if not module_summary_path.is_file():
        return None
    try:
        module_summary = load_json(module_summary_path)
    except json.JSONDecodeError:
        return None
    hicache_summary = extract_hicache_summary(module_summary)
    if not hicache_summary:
        return None

    transition_trace = hicache_summary.get("transition_trace")
    rows = transition_trace if isinstance(transition_trace, list) else []
    records = [prediction_record(row) for row in rows if isinstance(row, dict)]
    output_path = output_dir / "predicted_target_cache_state_trace.json"
    write_json(
        output_path,
        {
            "schema": "trace_sim.hicache.predicted_state_trace.v1",
            "source": "cpp_hicache_module",
            "record_count": len(records),
            "records": records,
            "final_state": hicache_summary.get("final_state", {}),
            "missing_state_model_facts": hicache_summary.get("missing_state_model_facts", {}),
            "skipped_non_state_model_events": hicache_summary.get("skipped_non_state_model_events", 0),
            "target_config": hicache_summary.get("target_config", {}),
        },
    )
    return output_path


def prediction_record(row: dict[str, Any]) -> dict[str, Any]:
    """Project one C++ transition row to the Python validation schema."""

    pages = [str(item) for item in row.get("pages", []) if item is not None]
    return {
        "request_id": row.get("request_id") or "",
        "operation_id": row.get("operation_id") or "",
        "source_fact_id": f"trace_event:{row.get('source_event_index', '')}",
        "source_event_index": row.get("source_event_index"),
        "source_event_name": row.get("event_name") or "",
        "cache_scope": row.get("cache_scope") or "",
        "ts": row.get("ts"),
        "event_base_name": event_base_name(str(row.get("event_name") or "")),
        "target_page_set": pages,
        "decision_kind": "state_prediction",
        "decision_reason": "derived_from_hicache_fact",
        "transition_kind": row.get("kind") or "",
        "tier_src": transition_source_tier(row),
        "tier_dst": transition_target_tier(row),
        "before_state_digest": row.get("before_state_digest") or "",
        "after_state_digest": row.get("after_state_digest") or "",
        "predicted_operation_kind": predicted_operation_kind(row),
        "blocking_class": "unknown",
        "unresolved_inputs": [],
    }


def transition_source_tier(row: dict[str, Any]) -> str:
    """Return the source tier represented by a remove transition."""

    kind = str(row.get("kind") or "")
    return str(row.get("tier") or "") if kind.startswith("remove_") else ""


def transition_target_tier(row: dict[str, Any]) -> str:
    """Return the destination tier represented by an add transition."""

    kind = str(row.get("kind") or "")
    return str(row.get("tier") or "") if kind.startswith("add_") else ""


def predicted_operation_kind(row: dict[str, Any]) -> str:
    """Map low-level state mutations to a stable operation category."""

    kind = str(row.get("kind") or "")
    if kind.startswith(("add_", "remove_")):
        return "resident_state_update"
    if kind.startswith(("mark_", "clear_")):
        return "page_metadata_update"
    return kind or "unknown"


def extract_hicache_summary(model_summary: Any) -> dict[str, Any]:
    """Extract the first structured HiCache result from a module summary."""

    if not isinstance(model_summary, dict):
        return {}
    modules = model_summary.get("modules")
    if not isinstance(modules, list):
        return {}
    for module in modules:
        if isinstance(module, dict) and isinstance(module.get("hicache"), dict):
            return module["hicache"]
    return {}
