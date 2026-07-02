"""把 predicted HiCache transition record replay 成可比较的 state delta。"""

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
    "enqueue_writeback",
}

LOCK_RELEASE_BY_KIND = {
    "release_request_ref": "request_ref",
    "complete_loadback": "loadback",
    "complete_storage_backup": "storage",
    "complete_write_through_backup": "write_through_backup",
    "complete_writeback": "writeback",
    "cancel_writeback": "writeback",
}


@dataclass
class PredictedTransitionReplay:
    """按 transition 顺序回放模型侧 active state。"""

    records: list[dict[str, Any]]
    sample_limit: int
    state: dict[str, set[str]] = field(default_factory=lambda: {key: set() for key in ACTIVE_STATE_KEYS})
    page_hit_counts: collections.Counter[str] = field(default_factory=collections.Counter)
    ref_counts: collections.Counter[str] = field(default_factory=collections.Counter)
    delta_rows: list[dict[str, Any]] = field(default_factory=list)
    violations: list[dict[str, Any]] = field(default_factory=list)
    ref_issues: list[dict[str, Any]] = field(default_factory=list)
    noop_rows: list[dict[str, Any]] = field(default_factory=list)

    def replay(self) -> dict[str, Any]:
        """执行 replay 并返回 final state、delta rows 和诊断。"""

        for ordinal, record in enumerate(self.records):
            self._replay_one(record, ordinal)

        final_state = {key: sorted(values) for key, values in self.state.items()}
        final_state["page_hit_counts"] = dict(sorted(self.page_hit_counts.items()))
        return {
            "final_state": final_state,
            "delta_rows": self.delta_rows,
            "state_constraint_violations": self.violations[: max(self.sample_limit, len(self.violations))],
            "ref_balance_issues": self.ref_issues[: max(self.sample_limit, len(self.ref_issues))],
            "derived_noop_transitions": self.noop_rows[: max(self.sample_limit, len(self.noop_rows))],
        }

    def _replay_one(self, record: dict[str, Any], ordinal: int) -> None:
        kind = str(record.get("transition_kind") or "")
        pages = record_pages(record)
        before_delta_count = len(self.delta_rows)
        if kind in {"add_l1_residency", "restore_l1_residency", "promote_visible_prefix_to_l1"}:
            add_pages(self.state, self.delta_rows, "l1_resident_pages", pages, record, ordinal)
            remove_pages(self.state, self.delta_rows, "evicted_pages", pages, record, ordinal, strict=False)
        elif kind == "mark_dirty":
            add_pages(self.state, self.delta_rows, "dirty_pages", pages, record, ordinal)
        elif kind in {"commit_host_storage_backup", "commit_host_backup"}:
            self._replay_host_backup(kind, pages, record, ordinal)
        elif kind == "apply_prefetch_host_visibility":
            self._replay_prefetch_host_visibility(pages, record, ordinal)
        elif kind == "evict_l1_node":
            self._replay_l1_evict(pages, record, ordinal)
        elif kind == "evict_host_node":
            self._replay_host_evict(pages, record, ordinal)
        elif kind == "prefetch_planned":
            add_pages(self.state, self.delta_rows, "prefetch_planned_pages", pages, record, ordinal)
        elif kind == "prefetch_ready":
            add_pages(self.state, self.delta_rows, "prefetch_ready_pages", pages, record, ordinal)
        elif kind in {"prefetch_revoked", "prefetch_suppressed"}:
            add_pages(self.state, self.delta_rows, "prefetch_suppressed_pages", pages, record, ordinal)
        elif kind == "prefetch_timeout_incomplete":
            add_pages(self.state, self.delta_rows, "prefetch_late_pages", pages, record, ordinal)
        elif kind == "prefetch_terminated":
            pass
        elif kind == "increment_hit_count":
            for page in pages:
                self.page_hit_counts[page] += 1
        elif kind == "enqueue_writeback":
            add_pages(self.state, self.delta_rows, "pending_writeback_pages", pages, record, ordinal)
            acquire_refs(self.state, self.delta_rows, self.ref_counts, pages, record, ordinal)
        elif kind in LOCK_ACQUIRE_KINDS:
            acquire_refs(self.state, self.delta_rows, self.ref_counts, pages, record, ordinal)
        elif kind in LOCK_RELEASE_BY_KIND:
            if kind in {"complete_writeback", "cancel_writeback"}:
                remove_pages(
                    self.state, self.delta_rows, "pending_writeback_pages", pages, record, ordinal, strict=False
                )
            release_refs(self.state, self.delta_rows, self.ref_counts, pages, record, ordinal, self.ref_issues)
        elif kind in {
            "enqueue_storage_backup",
            "enqueue_write_through_backup",
            "enqueue_loadback",
            "complete_storage_backup",
            "complete_loadback",
        }:
            pass
        elif kind:
            self.violations.append(
                {
                    "kind": "unknown_transition_kind",
                    "ordinal": ordinal,
                    "transition_kind": kind,
                    "source_event_index": record.get("source_event_index"),
                }
            )
        self._record_noop_if_needed(kind, pages, record, ordinal, before_delta_count)

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
        self.noop_rows.append(
            {
                "ordinal": ordinal,
                "transition_kind": kind,
                "source_event_index": record.get("source_event_index"),
                "page_count": len(pages),
            }
        )


def replay_predicted_records(records: list[dict[str, Any]], *, sample_limit: int) -> dict[str, Any]:
    """按 transition 顺序回放模型侧 active state。"""

    return PredictedTransitionReplay(records=records, sample_limit=sample_limit).replay()


def add_pages(
    state: dict[str, set[str]],
    delta_rows: list[dict[str, Any]],
    key: str,
    pages: list[str],
    record: dict[str, Any],
    ordinal: int,
) -> None:
    """向某个状态集合添加页，并记录实际 delta。"""

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
    violations: list[dict[str, Any]] | None = None,
) -> None:
    """从某个状态集合删除页，并记录实际 delta。"""

    missing = [page for page in pages if page not in state[key]]
    if strict and missing and violations is not None:
        violations.append(
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
    """按 page 维护简化 ref 计数。"""

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
    ref_issues: list[dict[str, Any]],
) -> None:
    """释放简化 ref 计数，并报告负计数风险。"""

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
        ref_issues.append(
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
    """记录 replay 产生的可比较状态 delta。"""

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
