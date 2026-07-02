"""HiCache state delta oracle 的共享常量。"""

DELTA_KIND_BY_STATE_KEY: dict[str, tuple[str, str]] = {
    "l1_resident_pages": ("add_l1_resident", "remove_l1_resident"),
    "l2_resident_pages": ("add_l2_resident", "remove_l2_resident"),
    "dirty_pages": ("mark_dirty", "clear_dirty"),
    "backuped_pages": ("mark_backuped", "clear_backuped"),
    "evicted_pages": ("mark_evicted", "clear_evicted"),
    "locked_pages": ("mark_locked", "clear_locked"),
}
