"""Schema helpers for predicted HiCache transition traces."""

from __future__ import annotations

from pathlib import Path
from typing import Any

from markov_internal.common.io import load_json


ACTIVE_STATE_KEYS = (
    "l1_resident_pages",
    "l2_resident_pages",
    "l3_resident_pages",
    "dirty_pages",
    "backuped_pages",
    "evicted_pages",
    "locked_pages",
    "pending_writeback_pages",
    "prefetch_planned_pages",
    "prefetch_ready_pages",
    "prefetch_late_pages",
    "prefetch_suppressed_pages",
)

SELF_CHECK_HARD_STATE_KEYS = tuple(key for key in ACTIVE_STATE_KEYS if key != "locked_pages")

STATE_DELTA_KINDS = {
    "l1_resident_pages": ("add_l1_resident", "remove_l1_resident"),
    "l2_resident_pages": ("add_l2_resident", "remove_l2_resident"),
    "l3_resident_pages": ("add_l3_resident", "remove_l3_resident"),
    "dirty_pages": ("mark_dirty", "clear_dirty"),
    "backuped_pages": ("mark_backuped", "clear_backuped"),
    "evicted_pages": ("mark_evicted", "clear_evicted"),
    "locked_pages": ("mark_locked", "clear_locked"),
    "pending_writeback_pages": ("mark_pending_writeback", "clear_pending_writeback"),
    "prefetch_planned_pages": ("prefetch_planned", "clear_prefetch_planned"),
    "prefetch_ready_pages": ("prefetch_ready", "clear_prefetch_ready"),
    "prefetch_late_pages": ("prefetch_late", "clear_prefetch_late"),
    "prefetch_suppressed_pages": ("prefetch_suppressed", "clear_prefetch_suppressed"),
}

KNOWN_TRANSITION_KINDS = {
    "add_l1_residency",
    "apply_prefetch_host_visibility",
    "cancel_writeback",
    "cache_extend_acquire_request_ref",
    "commit_host_backup",
    "commit_host_storage_backup",
    "complete_loadback",
    "complete_storage_backup",
    "complete_write_through_backup",
    "complete_writeback",
    "enqueue_loadback",
    "enqueue_storage_backup",
    "enqueue_write_through_backup",
    "enqueue_writeback",
    "evict_host_node",
    "evict_l1_node",
    "increment_hit_count",
    "mark_dirty",
    "prefetch_planned",
    "prefetch_ready",
    "prefetch_revoked",
    "prefetch_suppressed",
    "prefetch_terminated",
    "prefetch_timeout_incomplete",
    "promote_visible_prefix_to_l1",
    "release_request_ref",
    "restore_l1_residency",
}

CORE_RECORD_FIELDS = (
    "source_event_index",
    "source_event_name",
    "cache_scope",
    "target_page_set",
    "transition_kind",
)

ADVISORY_RECORD_FIELDS = (
    "request_id",
    "operation_id",
    "event_base_name",
    "predicted_operation_kind",
)


def load_predicted_trace(path: Path) -> dict[str, Any]:
    """Load a predicted state trace while preserving malformed fields for audit."""

    if not path.is_file():
        raise FileNotFoundError(f"missing predicted trace: {path}")
    payload = load_json(path)
    if not isinstance(payload, dict):
        raise ValueError(f"predicted trace is not a JSON object: {path}")
    return payload


def predicted_records(predicted: dict[str, Any]) -> list[dict[str, Any]]:
    """Return structured transition records from a predicted trace."""

    records = predicted.get("records")
    if not isinstance(records, list):
        return []
    return [record for record in records if isinstance(record, dict)]


def predicted_final_state(predicted: dict[str, Any]) -> dict[str, Any]:
    """Return the structured final state from a predicted trace."""

    final_state = predicted.get("final_state")
    return final_state if isinstance(final_state, dict) else {}


def record_pages(record: dict[str, Any]) -> list[str]:
    """Return normalized string pages from one transition record."""

    pages = record.get("target_page_set")
    if not isinstance(pages, list):
        return []
    return [str(page) for page in pages if page is not None]


def missing_core_field(record: dict[str, Any], field: str) -> bool:
    """Return whether a required transition field is structurally absent."""

    if field not in record:
        return True
    value = record.get(field)
    if field == "target_page_set":
        return not isinstance(value, list)
    return value is None


def state_counts(state: dict[str, Any]) -> dict[str, int]:
    """Count set-valued and page-counter fields in a state projection."""

    counts: dict[str, int] = {}
    for key, value in sorted(state.items()):
        if isinstance(value, list):
            counts[key] = len({str(item) for item in value if item is not None})
        elif isinstance(value, dict) and key == "page_hit_counts":
            counts[key] = len(value)
    return counts


def optional_int(value: Any, default: int = 0) -> int:
    """Parse an integer permissively while rejecting booleans."""

    if isinstance(value, bool):
        return default
    try:
        return int(float(value))
    except (TypeError, ValueError):
        return default
