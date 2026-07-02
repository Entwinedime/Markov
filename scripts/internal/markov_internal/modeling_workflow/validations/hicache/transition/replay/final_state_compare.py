"""predicted transition replay final state 自洽比较。"""

from __future__ import annotations

from typing import Any

from ...oracle.snapshot.state import normalize_hicache_page_key
from .record_schema import ACTIVE_STATE_KEYS, SELF_CHECK_HARD_STATE_KEYS, state_counts


def compare_replay_final_state(
    replay: dict[str, Any], final_state: dict[str, Any], *, sample_limit: int
) -> dict[str, Any]:
    """比较 replay final state 和模型 summary final state。"""

    replay_final = replay["final_state"]
    diffs: dict[str, Any] = {}
    strict_active_sets_match = True
    hard_active_sets_match = True
    for key in ACTIVE_STATE_KEYS:
        model_pages = normalize_page_set(final_state.get(key, []), "raw")
        replay_pages = normalize_page_set(replay_final.get(key, []), "raw")
        missing = sorted(model_pages - replay_pages)
        extra = sorted(replay_pages - model_pages)
        if missing or extra:
            strict_active_sets_match = False
            if key in SELF_CHECK_HARD_STATE_KEYS:
                hard_active_sets_match = False
        diffs[key] = {
            "match": not missing and not extra,
            "model_count": len(model_pages),
            "replayed_count": len(replay_pages),
            "missing_in_replay": missing[:sample_limit],
            "extra_in_replay": extra[:sample_limit],
            "missing_count": len(missing),
            "extra_count": len(extra),
        }
    model_hits = (
        {str(key): int(value) for key, value in (final_state.get("page_hit_counts") or {}).items()}
        if isinstance(final_state.get("page_hit_counts"), dict)
        else {}
    )
    replay_hits = (
        {str(key): int(value) for key, value in (replay_final.get("page_hit_counts") or {}).items()}
        if isinstance(replay_final.get("page_hit_counts"), dict)
        else {}
    )
    hit_mismatch = compare_counter_dicts(model_hits, replay_hits, sample_limit=sample_limit)
    return {
        "replay_final_state_match": hard_active_sets_match,
        "active_set_replay_match": hard_active_sets_match,
        "strict_active_set_replay_match": strict_active_sets_match,
        "strict_replay_final_state_match": strict_active_sets_match and hit_mismatch["match"],
        "page_hit_counts_match": hit_mismatch["match"],
        "replayed_final_state_counts": state_counts(replay_final),
        "model_final_state_counts": state_counts(final_state),
        "sets_diff_by_tier": diffs,
        "page_hit_counts_diff": hit_mismatch,
        "unreplayed_state_transition_count": 0,
        "advisory_replay_state_keys": {
            "locked_pages": "predicted transition trace does not currently expose every source_actual lock/ref or prefetch anchor protection mutation; strict mismatch is reported but stable state exactness does not gate on it.",
            "page_hit_counts": "hit count is diagnostic metadata and is reported separately from active-state replay.",
        },
    }


def normalize_page_set(value: Any, page_key_mode: str) -> set[str]:
    """把页面列表归一化成集合。"""

    if not isinstance(value, list):
        return set()
    return {normalize_hicache_page_key(page, page_key_mode) for page in value if page is not None}


def compare_counter_dicts(expected: dict[str, int], actual: dict[str, int], *, sample_limit: int) -> dict[str, Any]:
    """比较两个 page counter 字典。"""

    mismatches: list[dict[str, Any]] = []
    for key in sorted(set(expected) | set(actual)):
        left = expected.get(key, 0)
        right = actual.get(key, 0)
        if left != right:
            mismatches.append({"page": key, "model_count": left, "replayed_count": right})
    return {
        "match": not mismatches,
        "mismatch_count": len(mismatches),
        "mismatches": mismatches[:sample_limit],
    }
