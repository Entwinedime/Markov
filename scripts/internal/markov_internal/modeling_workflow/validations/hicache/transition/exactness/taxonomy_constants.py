"""HiCache transition taxonomy 共享常量。"""

OBSERVED_ROLE_TO_OPERATION_KIND = {
    "capacity_request": "capacity_request",
    "capacity_result_observed": "capacity_result",
    "insert_result_observed": "request_insert",
    "writeback_enqueue_observed": "write_back_flush",
    "writeback_io_observed": "write_back_flush",
    "prefetch_decision_observed": "prefetch_plan",
    "prefetch_intent_observed": "prefetch_plan",
    "prefetch_progress_observed": "prefetch_ready",
    "lock_scope_delta": "lock_ref",
    "lock_scope_result_observed": "lock_ref",
    "request_admission_observed": "scheduler_admission_observed",
}

MARKER_DELTA_KINDS = {
    "mark_dirty",
    "clear_dirty",
    "mark_backuped",
    "clear_backuped",
    "mark_evicted",
    "clear_evicted",
    "mark_locked",
    "clear_locked",
    "mark_pending_writeback",
    "clear_pending_writeback",
}

PREFETCH_DELTA_KINDS = {
    "prefetch_planned",
    "clear_prefetch_planned",
    "prefetch_ready",
    "clear_prefetch_ready",
    "prefetch_late",
    "clear_prefetch_late",
    "prefetch_suppressed",
    "clear_prefetch_suppressed",
}

HOST_VISIBLE_DELTA_KINDS = {
    "add_l2_resident",
    "remove_l2_resident",
    "add_l3_resident",
    "remove_l3_resident",
    "mark_backuped",
    "clear_backuped",
}

DEVICE_VISIBLE_DELTA_KINDS = {
    "add_l1_resident",
    "remove_l1_resident",
}

TRANSITION_PAGE_FIELDS = (
    "pages",
    "target_page_set",
    "host_pages",
    "lock_pages",
    "prefix_pages",
    "suffix_pages",
    "hit_pages",
)

STATE_ONLY_OPERATION_KINDS = {
    "dirty_marker",
    "evicted_marker",
    "backuped_marker",
    "ref_protection",
    "hit_count_update",
    "allocator_pressure",
    "prefetch_control",
    "prefetch_plan",
    "prefetch_revoke",
    "snapshot_delta_marker",
}

PHYSICAL_CANDIDATE_OPERATION_KINDS = {
    "host_backup",
    "storage_backup",
    "write_back_flush",
    "device_loadback",
    "prefetch_read",
    "prefetch_apply",
    "host_cleanup",
    "device_eviction",
}
