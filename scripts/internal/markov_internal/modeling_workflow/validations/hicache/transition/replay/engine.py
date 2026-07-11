"""Replay predicted HiCache transition records into comparable state deltas."""

from __future__ import annotations

import collections
from dataclasses import dataclass, field
from typing import Any

from .record_schema import (
    ACTIVE_STATE_KEYS,
    STATE_DELTA_KINDS,
    record_pages,
)


LOCK_ACQUIRE_KINDS = {
    "cache_extend_acquire_request_ref",
    "enqueue_loadback",
    "enqueue_storage_backup",
    "enqueue_write_through_backup",
}

LOCK_RELEASE_KINDS = {
    "release_request_ref",
    "complete_loadback",
    "complete_storage_backup",
    "complete_write_through_backup",
    "complete_writeback",
    "cancel_writeback",
}

L1_RESIDENCY_KINDS = {
    "add_l1_residency",
    "restore_l1_residency",
    "promote_visible_prefix_to_l1",
}

PREFETCH_STATE_KEY_BY_KIND = {
    "prefetch_planned": "prefetch_planned_pages",
    "prefetch_ready": "prefetch_ready_pages",
    "prefetch_revoked": "prefetch_suppressed_pages",
    "prefetch_suppressed": "prefetch_suppressed_pages",
    "prefetch_timeout_incomplete": "prefetch_late_pages",
}


@dataclass
class DiagnosticSamples:
    """Count every diagnostic while retaining only a bounded row sample."""

    sample_limit: int
    count: int = 0
    rows: list[dict[str, Any]] = field(default_factory=list)

    def add(self, row: dict[str, Any]) -> None:
        """Record one occurrence and retain it when sample capacity remains."""

        self.count += 1
        if len(self.rows) < self.sample_limit:
            self.rows.append(row)


@dataclass
class PredictedTransitionReplay:
    """Replay model-side active state in transition order."""

    records: list[dict[str, Any]]
    sample_limit: int
    state: dict[str, set[str]] = field(default_factory=lambda: {key: set() for key in ACTIVE_STATE_KEYS})
    page_hit_counts: collections.Counter[str] = field(default_factory=collections.Counter)
    ref_counts: collections.Counter[str] = field(default_factory=collections.Counter)
    delta_rows: list[dict[str, Any]] = field(default_factory=list)
    violations: DiagnosticSamples = field(init=False)
    ref_issues: DiagnosticSamples = field(init=False)
    noop_rows: DiagnosticSamples = field(init=False)

    def __post_init__(self) -> None:
        """Create bounded diagnostic collectors after `sample_limit` is available."""

        self.violations = DiagnosticSamples(self.sample_limit)
        self.ref_issues = DiagnosticSamples(self.sample_limit)
        self.noop_rows = DiagnosticSamples(self.sample_limit)

    def replay(self) -> dict[str, Any]:
        """Execute replay and return final state, deltas, and diagnostics."""

        for ordinal, record in enumerate(self.records):
            self._replay_one(record, ordinal)

        final_state = {key: sorted(values) for key, values in self.state.items()}
        final_state["page_hit_counts"] = dict(sorted(self.page_hit_counts.items()))
        return {
            "final_state": final_state,
            "delta_rows": self.delta_rows,
            "state_constraint_violation_count": self.violations.count,
            "state_constraint_violations": self.violations.rows,
            "ref_balance_issue_count": self.ref_issues.count,
            "ref_balance_issues": self.ref_issues.rows,
            "derived_noop_transition_count": self.noop_rows.count,
            "derived_noop_transitions": self.noop_rows.rows,
        }

    def _replay_one(self, record: dict[str, Any], ordinal: int) -> None:
        kind = str(record.get("transition_kind") or "")
        pages = record_pages(record)
        before_delta_count = len(self.delta_rows)
        handled = (
            self._replay_residency_or_storage(kind, pages, record, ordinal)
            or self._replay_prefetch(kind, pages, record, ordinal)
            or self._replay_reference_lifecycle(kind, pages, record, ordinal)
        )
        if not handled and kind:
            self.violations.add(
                {
                    "kind": "unknown_transition_kind",
                    "ordinal": ordinal,
                    "transition_kind": kind,
                    "source_event_index": record.get("source_event_index"),
                }
            )
        self._record_noop_if_needed(kind, pages, record, ordinal, before_delta_count)

    def _replay_residency_or_storage(
        self,
        kind: str,
        pages: list[str],
        record: dict[str, Any],
        ordinal: int,
    ) -> bool:
        """Replay residency, backup visibility, and eviction transitions."""

        if kind in L1_RESIDENCY_KINDS:
            add_pages(self.state, self.delta_rows, "l1_resident_pages", pages, record, ordinal)
            remove_pages(self.state, self.delta_rows, "evicted_pages", pages, record, ordinal, strict=False)
        elif kind == "mark_dirty":
            add_pages(self.state, self.delta_rows, "dirty_pages", pages, record, ordinal)
        elif kind in {"commit_host_storage_backup", "commit_host_backup"}:
            self._replay_host_backup(kind, pages, record, ordinal)
        elif kind == "evict_l1_node":
            self._replay_l1_evict(pages, record, ordinal)
        elif kind == "evict_host_node":
            self._replay_host_evict(pages, record, ordinal)
        else:
            return False
        return True

    def _replay_prefetch(
        self,
        kind: str,
        pages: list[str],
        record: dict[str, Any],
        ordinal: int,
    ) -> bool:
        """Replay prefetch visibility and diagnostic lifecycle markers."""

        if kind == "apply_prefetch_host_visibility":
            self._replay_prefetch_host_visibility(pages, record, ordinal)
            return True
        state_key = PREFETCH_STATE_KEY_BY_KIND.get(kind)
        if state_key is not None:
            add_pages(self.state, self.delta_rows, state_key, pages, record, ordinal)
            return True
        return kind == "prefetch_terminated"

    def _replay_reference_lifecycle(
        self,
        kind: str,
        pages: list[str],
        record: dict[str, Any],
        ordinal: int,
    ) -> bool:
        """Replay hit counters, request references, and writeback references."""

        if kind == "increment_hit_count":
            for page in pages:
                self.page_hit_counts[page] += 1
        elif kind == "enqueue_writeback":
            add_pages(self.state, self.delta_rows, "pending_writeback_pages", pages, record, ordinal)
            acquire_refs(self.state, self.delta_rows, self.ref_counts, pages, record, ordinal)
        elif kind in LOCK_ACQUIRE_KINDS:
            acquire_refs(self.state, self.delta_rows, self.ref_counts, pages, record, ordinal)
        elif kind in LOCK_RELEASE_KINDS:
            if kind in {"complete_writeback", "cancel_writeback"}:
                remove_pages(
                    self.state, self.delta_rows, "pending_writeback_pages", pages, record, ordinal, strict=False
                )
            release_refs(self.state, self.delta_rows, self.ref_counts, pages, record, ordinal, self.ref_issues)
        else:
            return False
        return True

    def _replay_host_backup(self, kind: str, pages: list[str], record: dict[str, Any], ordinal: int) -> None:
        add_pages(self.state, self.delta_rows, "l2_resident_pages", pages, record, ordinal)
        add_pages(self.state, self.delta_rows, "backuped_pages", pages, record, ordinal)
        if kind == "commit_host_storage_backup":
            add_pages(self.state, self.delta_rows, "l3_resident_pages", pages, record, ordinal)
        remove_pages(self.state, self.delta_rows, "dirty_pages", pages, record, ordinal, strict=False)

    def _replay_prefetch_host_visibility(self, pages: list[str], record: dict[str, Any], ordinal: int) -> None:
        add_pages(self.state, self.delta_rows, "l2_resident_pages", pages, record, ordinal)
        add_pages(self.state, self.delta_rows, "backuped_pages", pages, record, ordinal)
        add_pages(self.state, self.delta_rows, "l3_resident_pages", pages, record, ordinal)
        add_pages(
            self.state,
            self.delta_rows,
            "evicted_pages",
            [page for page in pages if page not in self.state["l1_resident_pages"]],
            record,
            ordinal,
        )

    def _replay_l1_evict(self, pages: list[str], record: dict[str, Any], ordinal: int) -> None:
        remove_pages(
            self.state,
            self.delta_rows,
            "l1_resident_pages",
            pages,
            record,
            ordinal,
            strict=True,
            violations=self.violations,
        )
        remove_pages(self.state, self.delta_rows, "dirty_pages", pages, record, ordinal, strict=False)
        backed_pages = [
            page for page in pages if page in self.state["l2_resident_pages"] or page in self.state["backuped_pages"]
        ]
        add_pages(self.state, self.delta_rows, "evicted_pages", backed_pages, record, ordinal)

    def _replay_host_evict(self, pages: list[str], record: dict[str, Any], ordinal: int) -> None:
        for key in ("l2_resident_pages", "l3_resident_pages", "backuped_pages", "evicted_pages", "locked_pages"):
            remove_pages(self.state, self.delta_rows, key, pages, record, ordinal, strict=False)
        for page in pages:
            self.ref_counts.pop(page, None)

    def _record_noop_if_needed(
        self, kind: str, pages: list[str], record: dict[str, Any], ordinal: int, before_delta_count: int
    ) -> None:
        if (
            len(self.delta_rows) != before_delta_count
            or not pages
            or kind
            in {
                "increment_hit_count",
                "complete_storage_backup",
                "complete_loadback",
                "complete_writeback",
                "cancel_writeback",
                "prefetch_terminated",
            }
        ):
            return
        self.noop_rows.add(
            {
                "ordinal": ordinal,
                "transition_kind": kind,
                "source_event_index": record.get("source_event_index"),
                "page_count": len(pages),
            }
        )


def replay_predicted_records(records: list[dict[str, Any]], *, sample_limit: int) -> dict[str, Any]:
    """Replay model-side active state in transition order."""

    return PredictedTransitionReplay(records=records, sample_limit=sample_limit).replay()


def add_pages(
    state: dict[str, set[str]],
    delta_rows: list[dict[str, Any]],
    key: str,
    pages: list[str],
    record: dict[str, Any],
    ordinal: int,
) -> None:
    """Add pages to a state set and record the effective delta."""

    changed = [page for page in pages if page not in state[key]]
    if not changed:
        return
    state[key].update(changed)
    emit_delta(delta_rows, key, True, changed, record, ordinal)


def remove_pages(
    state: dict[str, set[str]],
    delta_rows: list[dict[str, Any]],
    key: str,
    pages: list[str],
    record: dict[str, Any],
    ordinal: int,
    *,
    strict: bool,
    violations: DiagnosticSamples | None = None,
) -> None:
    """Remove pages from a state set and record the effective delta."""

    missing = [page for page in pages if page not in state[key]]
    if strict and missing and violations is not None:
        violations.add(
            {
                "kind": "remove_missing_page",
                "state_key": key,
                "ordinal": ordinal,
                "transition_kind": record.get("transition_kind"),
                "source_event_index": record.get("source_event_index"),
                "missing_pages_sample": missing[:8],
                "missing_count": len(missing),
            }
        )
    changed = [page for page in pages if page in state[key]]
    if not changed:
        return
    state[key].difference_update(changed)
    emit_delta(delta_rows, key, False, changed, record, ordinal)


def acquire_refs(
    state: dict[str, set[str]],
    delta_rows: list[dict[str, Any]],
    ref_counts: collections.Counter[str],
    pages: list[str],
    record: dict[str, Any],
    ordinal: int,
) -> None:
    """Acquire simplified page references and expose newly locked pages."""

    newly_locked: list[str] = []
    for page in pages:
        ref_counts[page] += 1
        if ref_counts[page] == 1 and page not in state["locked_pages"]:
            state["locked_pages"].add(page)
            newly_locked.append(page)
    if newly_locked:
        emit_delta(delta_rows, "locked_pages", True, newly_locked, record, ordinal)


def release_refs(
    state: dict[str, set[str]],
    delta_rows: list[dict[str, Any]],
    ref_counts: collections.Counter[str],
    pages: list[str],
    record: dict[str, Any],
    ordinal: int,
    ref_issues: DiagnosticSamples,
) -> None:
    """Release simplified page references and report unmatched releases."""

    cleared: list[str] = []
    missing: list[str] = []
    for page in pages:
        if ref_counts[page] <= 0:
            missing.append(page)
            ref_counts.pop(page, None)
            continue
        ref_counts[page] -= 1
        if ref_counts[page] <= 0:
            ref_counts.pop(page, None)
            if page in state["locked_pages"]:
                state["locked_pages"].remove(page)
                cleared.append(page)
    if missing:
        ref_issues.add(
            {
                "kind": "release_ref_without_replay_acquire",
                "ordinal": ordinal,
                "transition_kind": record.get("transition_kind"),
                "source_event_index": record.get("source_event_index"),
                "missing_pages_sample": missing[:8],
                "missing_count": len(missing),
            }
        )
    if cleared:
        emit_delta(delta_rows, "locked_pages", False, cleared, record, ordinal)


def emit_delta(
    delta_rows: list[dict[str, Any]],
    state_key: str,
    is_add: bool,
    pages: list[str],
    record: dict[str, Any],
    ordinal: int,
) -> None:
    """Record one comparable state delta produced by replay."""

    kinds = STATE_DELTA_KINDS.get(state_key)
    if kinds is None or not pages:
        return
    transition_kind = kinds[0] if is_add else kinds[1]
    delta_rows.append(
        {
            "transition_ordinal": ordinal,
            "source_transition_kind": record.get("transition_kind"),
            "transition_kind": transition_kind,
            "state_key": state_key,
            "pages": sorted(set(pages)),
            "cache_scope": record.get("cache_scope") or "",
            "request_id": record.get("request_id") or "",
            "operation_id": record.get("operation_id") or "",
            "source_event_index": record.get("source_event_index"),
            "source_event_name": record.get("source_event_name") or "",
            "event_base_name": record.get("event_base_name") or "",
            "ts": record.get("ts"),
        }
    )
