#!/usr/bin/env python3
"""Fixtures for the C++ HiCache token-invariant state model."""

from __future__ import annotations

import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
HASH_ALGO = "sglang_sha256_parent_u32le"


def main() -> int:
    with tempfile.TemporaryDirectory() as raw_tmp:
        tmp = Path(raw_tmp)
        run_observed_policy_rejected_fixture(tmp)
        run_token_insert_write_through_fixture(tmp)
        run_lru_capacity_from_token_pages_fixture(tmp)
        run_lock_scope_capacity_fixture(tmp)
        run_prefetch_wait_complete_fixture(tmp)
        run_prefetch_best_effort_suppressed_fixture(tmp)
        run_non_invariant_observations_skipped_fixture(tmp)
        run_missing_dictionary_reported_fixture(tmp)
    print("hicache state fixtures passed")
    return 0


def run_observed_policy_rejected_fixture(tmp: Path) -> None:
    trace_path = tmp / "hicache_observed_policy_rejected_trace.json"
    trace_path.write_text(json.dumps({"traceEvents": []}, ensure_ascii=True), encoding="utf-8")
    for index, hicache_config in enumerate(
        (
            {"enabled": True, "write_policy": "observed"},
            {"enabled": True, "prefetch_policy": "observed"},
        )
    ):
        model_config = tmp / f"hicache_observed_policy_rejected_{index}.json"
        summary_out = tmp / f"hicache_observed_policy_rejected_{index}_summary.json"
        run_summary = tmp / f"hicache_observed_policy_rejected_{index}_run.json"
        model_config.write_text(json.dumps({"modules": ["hicache"], "hicache": hicache_config}, ensure_ascii=True), encoding="utf-8")
        result = subprocess.run(
            [
                str(ROOT / "build/bin/trace_graph"),
                "--input",
                str(trace_path),
                "--run-summary",
                str(run_summary),
                "--model-config",
                str(model_config),
                "--model-summary",
                str(summary_out),
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        assert result.returncode != 0, result.stdout + result.stderr
        assert "observed" in result.stderr or "observed" in result.stdout, result.stdout + result.stderr


def run_token_insert_write_through_fixture(tmp: Path) -> None:
    page_size = 4
    scope = "rank0"
    tokens = [11, 12, 13, 14, 21, 22, 23, 24]
    pages = page_ids(tokens, page_size, scope)
    events = [
        invariant_event(
            10,
            "request_tokens",
            "hicache_controller.request_tokens",
            {
                "request_id": "req-insert",
                "cache_scope": scope,
                "source_page_size": page_size,
                "seq_no": 1,
                "token_dictionary": token_dictionary("path-insert", tokens),
                "full_path_span": token_span("path-insert", 0, len(tokens)),
                "token_count": len(tokens),
            },
        ),
        invariant_event(
            20,
            "lookup_path",
            "hiradix.match_prefix",
            {
                "request_id": "req-insert",
                "cache_scope": scope,
                "source_page_size": page_size,
                "seq_no": 2,
                "token_dictionary": token_dictionary("path-insert", tokens),
                "full_path_span": token_span("path-insert", 0, len(tokens)),
                "matched_token_len": 0,
            },
        ),
        invariant_event(
            30,
            "insert_path",
            "hiradix.insert",
            {
                "request_id": "req-insert",
                "cache_scope": scope,
                "source_page_size": page_size,
                "seq_no": 3,
                "token_dictionary": token_dictionary("path-insert", tokens),
                "full_path_span": token_span("path-insert", 0, len(tokens)),
                "value_token_count": len(tokens),
                "prefix_len": 0,
            },
        ),
    ]
    summary = run_hicache_trace(
        tmp,
        "token_insert_write_through",
        events,
        {"page_size": page_size, "write_policy": "write_through"},
    )

    final_state = summary["final_state"]
    assert summary["input_hicache_events"] == 3, summary
    assert summary["processed_hicache_events"] == 3, summary
    assert summary["skipped_non_invariant_events"] == 0, summary
    assert summary["missing_invariant_facts"] == {}, summary
    sorted_pages = sorted(pages)
    assert final_state["l1_resident_pages"] == sorted_pages, final_state
    assert final_state["l2_resident_pages"] == sorted_pages, final_state
    assert final_state["l3_resident_pages"] == sorted_pages, final_state
    assert final_state["dirty_pages"] == [], final_state
    assert final_state["backuped_pages"] == sorted_pages, final_state
    assert summary["processed_events_by_role"] == {"insert_path": 1, "lookup_path": 1, "request_tokens": 1}, summary
    assert summary["transitions_by_kind"]["add_l1_resident"] == 2, summary
    assert summary["transitions_by_kind"]["add_l2_resident"] == 2, summary
    assert summary["transitions_by_kind"]["add_l3_resident"] == 2, summary


def run_lru_capacity_from_token_pages_fixture(tmp: Path) -> None:
    page_size = 4
    scope = "rank0"
    path_a = [101, 102, 103, 104]
    path_b = [201, 202, 203, 204]
    path_c = [301, 302, 303, 304]
    page_a = page_ids(path_a, page_size, scope)[0]
    page_b = page_ids(path_b, page_size, scope)[0]
    page_c = page_ids(path_c, page_size, scope)[0]
    events = [
        insert_event(10, "req-a", "path-a", path_a, 1, page_size, scope),
        insert_event(20, "req-b", "path-b", path_b, 2, page_size, scope),
        invariant_event(
            30,
            "lookup_path",
            "hiradix.match_prefix",
            {
                "request_id": "req-a",
                "cache_scope": scope,
                "source_page_size": page_size,
                "seq_no": 3,
                "token_dictionary": token_dictionary("path-a", path_a),
                "full_path_span": token_span("path-a", 0, len(path_a)),
                "matched_token_len": page_size,
            },
        ),
        insert_event(40, "req-c", "path-c", path_c, 4, page_size, scope),
    ]
    summary = run_hicache_trace(
        tmp,
        "lru_capacity_from_token_pages",
        events,
        {"page_size": page_size, "l1_capacity_pages": 2, "write_policy": "write_through"},
    )

    final_state = summary["final_state"]
    assert final_state["l1_resident_pages"] == sorted([page_a, page_c]), final_state
    assert final_state["l2_resident_pages"] == sorted([page_a, page_b, page_c]), final_state
    assert final_state["l3_resident_pages"] == sorted([page_a, page_b, page_c]), final_state
    assert final_state["evicted_pages"] == [page_b], final_state
    assert summary["transitions_by_kind"]["mark_evicted"] == 1, summary
    assert summary["dirty_eviction_events"] == 0, summary


def run_lock_scope_capacity_fixture(tmp: Path) -> None:
    page_size = 4
    scope = "rank0"
    path_a = [401, 402, 403, 404]
    path_b = [501, 502, 503, 504]
    page_a = page_ids(path_a, page_size, scope)[0]
    page_b = page_ids(path_b, page_size, scope)[0]
    events = [
        insert_event(10, "req-lock-a", "path-lock-a", path_a, 1, page_size, scope),
        insert_event(20, "req-lock-b", "path-lock-b", path_b, 2, page_size, scope),
        invariant_event(
            30,
            "lock_scope_delta",
            "hiradix.inc_lock_ref",
            {
                "request_id": "req-lock-a",
                "operation_id": "lock-a",
                "cache_scope": scope,
                "source_page_size": page_size,
                "seq_no": 3,
                "node_token_dictionary": token_dictionary("path-lock-a", path_a),
                "logical_path_span": token_span("path-lock-a", 0, len(path_a)),
                "delta": 1,
                "lock_direction": "inc",
            },
        ),
        invariant_event(
            40,
            "capacity_request",
            "hicache_controller.evict",
            {
                "cache_scope": scope,
                "source_page_size": page_size,
                "seq_no": 4,
                "requested_tokens": page_size,
                "requested_pages_source": {"requested_pages": 1, "requested_tokens": page_size},
                "reason": "fixture",
                "tier": "L1",
                "policy_params": {"policy": "lru"},
            },
        ),
    ]
    summary = run_hicache_trace(
        tmp,
        "lock_scope_capacity",
        events,
        {"page_size": page_size, "l1_capacity_pages": 2, "write_policy": "write_through"},
    )

    final_state = summary["final_state"]
    assert summary["lock_state_events"] == 1, summary
    assert final_state["l1_resident_pages"] == sorted([page_a, page_b]), final_state
    assert final_state["locked_pages"] == [page_a], final_state
    assert final_state["evicted_pages"] == [], final_state
    assert summary["transitions_by_kind"]["mark_locked"] == 1, summary
    assert summary["transitions_by_kind"].get("mark_evicted", 0) == 0, summary


def run_prefetch_wait_complete_fixture(tmp: Path) -> None:
    page_size = 4
    scope = "rank0"
    prefix = [601, 602, 603, 604]
    suffix = [701, 702, 703, 704, 801, 802, 803, 804]
    full = prefix + suffix
    prefix_page = page_ids(prefix, page_size, scope)[0]
    suffix_pages = page_ids(full, page_size, scope)[1:]
    events = [
        insert_event(10, "req-prefix", "path-prefix", prefix, 1, page_size, scope),
        prefetch_intent_event(20, "req-prefetch", "prefetch-full", prefix, suffix, 2, page_size, scope),
        invariant_event(
            30,
            "prefetch_check_point",
            "hiradix.check_prefetch_progress",
            {
                "request_id": "req-prefetch",
                "operation_id": "prefetch-check",
                "cache_scope": scope,
                "source_page_size": page_size,
                "seq_no": 3,
                "check_kind": "wait_complete",
            },
        ),
    ]
    summary = run_hicache_trace(
        tmp,
        "prefetch_wait_complete",
        events,
        {"page_size": page_size, "write_policy": "write_through", "prefetch_policy": "wait_complete"},
    )

    final_state = summary["final_state"]
    sorted_suffix_pages = sorted(suffix_pages)
    assert final_state["prefetch_planned_pages"] == sorted_suffix_pages, final_state
    assert final_state["prefetch_ready_pages"] == [], final_state
    assert final_state["prefetch_suppressed_pages"] == [], final_state
    assert final_state["l2_resident_pages"] == [prefix_page], final_state
    assert final_state["l3_resident_pages"] == [prefix_page], final_state
    assert summary["transitions_by_kind"]["mark_prefetch_planned"] == 2, summary
    assert summary["transitions_by_kind"].get("mark_prefetch_ready", 0) == 0, summary


def run_prefetch_best_effort_suppressed_fixture(tmp: Path) -> None:
    page_size = 4
    scope = "rank0"
    prefix = [901, 902, 903, 904]
    suffix = [1001, 1002, 1003, 1004]
    full = prefix + suffix
    suffix_pages = page_ids(full, page_size, scope)[1:]
    events = [
        prefetch_intent_event(10, "req-best-effort", "best-effort-full", prefix, suffix, 1, page_size, scope),
        invariant_event(
            20,
            "prefetch_check_point",
            "hiradix.check_prefetch_progress",
            {
                "request_id": "req-best-effort",
                "operation_id": "best-effort-check",
                "cache_scope": scope,
                "source_page_size": page_size,
                "seq_no": 2,
                "check_kind": "poll",
            },
        ),
    ]
    summary = run_hicache_trace(
        tmp,
        "prefetch_best_effort_suppressed",
        events,
        {"page_size": page_size, "write_policy": "write_through", "prefetch_policy": "best_effort"},
    )

    final_state = summary["final_state"]
    assert final_state["prefetch_planned_pages"] == suffix_pages, final_state
    assert final_state["prefetch_ready_pages"] == [], final_state
    assert final_state["prefetch_suppressed_pages"] == suffix_pages, final_state
    assert final_state["l2_resident_pages"] == [], final_state
    assert final_state["l3_resident_pages"] == [], final_state
    assert summary["transitions_by_kind"]["mark_prefetch_suppressed"] == 1, summary


def run_non_invariant_observations_skipped_fixture(tmp: Path) -> None:
    page_size = 4
    scope = "rank0"
    tokens = [1101, 1102, 1103, 1104]
    events = [
        observation_event(
            10,
            "prefetch_io_observed",
            "timing_observation",
            "hicache_controller.prefetch_io_observed",
            {
                "operation_id": "prefetch-io",
                "cache_scope": scope,
                "source_page_size": page_size,
                "seq_no": 1,
                "token_dictionary": token_dictionary("io-path", tokens),
                "token_span": token_span("io-path", 0, len(tokens)),
                "completed_tokens": len(tokens),
                "bytes": 1024,
            },
        ),
        observation_event(
            20,
            "writeback_io_observed",
            "timing_observation",
            "hicache_controller.writeback_io_observed",
            {
                "operation_id": "writeback-io",
                "cache_scope": scope,
                "source_page_size": page_size,
                "seq_no": 2,
                "token_dictionary": token_dictionary("io-path", tokens),
                "token_span": token_span("io-path", 0, len(tokens)),
                "completed_tokens": len(tokens),
                "bytes": 1024,
            },
        ),
        observation_event(
            30,
            "writeback_enqueue_observed",
            "source_actual",
            "hicache_controller.writeback_enqueue_observed",
            {
                "operation_id": "writeback-enqueue",
                "cache_scope": scope,
                "source_page_size": page_size,
                "seq_no": 3,
                "token_dictionary": token_dictionary("io-path", tokens),
                "token_span": token_span("io-path", 0, len(tokens)),
            },
        ),
    ]
    summary = run_hicache_trace(tmp, "non_invariant_observations_skipped", events, {"page_size": page_size})

    final_state = summary["final_state"]
    assert summary["input_hicache_events"] == 3, summary
    assert summary["processed_hicache_events"] == 0, summary
    assert summary["skipped_non_invariant_events"] == 3, summary
    assert summary["non_invariant_fact_usage"] == [], summary
    assert final_state["l1_resident_pages"] == [], final_state
    assert final_state["l2_resident_pages"] == [], final_state
    assert final_state["l3_resident_pages"] == [], final_state
    assert final_state["dirty_pages"] == [], final_state


def run_missing_dictionary_reported_fixture(tmp: Path) -> None:
    page_size = 4
    scope = "rank0"
    events = [
        invariant_event(
            10,
            "insert_path",
            "hiradix.insert",
            {
                "request_id": "req-missing-dict",
                "cache_scope": scope,
                "source_page_size": page_size,
                "seq_no": 1,
                "full_path_span": token_span("missing-path", 0, page_size),
                "token_count": page_size,
                "value_token_count": page_size,
                "prefix_len": 0,
            },
        )
    ]
    summary = run_hicache_trace(tmp, "missing_dictionary_reported", events, {"page_size": page_size})

    assert summary["input_hicache_events"] == 1, summary
    assert summary["processed_hicache_events"] == 1, summary
    assert summary["missing_invariant_facts"] == {"token_dictionary_or_full_path_span": 1}, summary
    assert summary["state_transition_count"] == 0, summary
    assert summary["final_state"]["l1_resident_pages"] == [], summary


def insert_event(ts: int, request_id: str, path_id: str, tokens: list[int], seq_no: int, page_size: int, scope: str) -> dict[str, Any]:
    return invariant_event(
        ts,
        "insert_path",
        "hiradix.insert",
        {
            "request_id": request_id,
            "cache_scope": scope,
            "source_page_size": page_size,
            "seq_no": seq_no,
            "token_dictionary": token_dictionary(path_id, tokens),
            "full_path_span": token_span(path_id, 0, len(tokens)),
            "token_count": len(tokens),
            "value_token_count": len(tokens),
            "prefix_len": 0,
        },
    )


def prefetch_intent_event(
    ts: int,
    request_id: str,
    path_prefix: str,
    prefix: list[int],
    suffix: list[int],
    seq_no: int,
    page_size: int,
    scope: str,
) -> dict[str, Any]:
    full = prefix + suffix
    full_id = f"{path_prefix}-full"
    prefix_id = f"{path_prefix}-prefix"
    suffix_id = f"{path_prefix}-suffix"
    return invariant_event(
        ts,
        "prefetch_intent",
        "hiradix.prefetch_from_storage",
        {
            "request_id": request_id,
            "operation_id": f"{request_id}-intent",
            "cache_scope": scope,
            "source_page_size": page_size,
            "seq_no": seq_no,
            "prefix_token_dictionary": token_dictionary(prefix_id, prefix),
            "suffix_token_dictionary": token_dictionary(suffix_id, suffix),
            "full_token_dictionary": token_dictionary(full_id, full),
            "prefix_span": token_span(prefix_id, 0, len(prefix)),
            "suffix_span": token_span(suffix_id, 0, len(suffix)),
            "full_path_span": token_span(full_id, 0, len(full)),
            "policy_params": {"policy": "target"},
            "requested_tokens": len(suffix),
        },
    )


def invariant_event(ts: int, role: str, target_id: str, args: dict[str, Any]) -> dict[str, Any]:
    return hicache_event(
        ts,
        f"hicache_{role}_end",
        {
            "target_id": target_id,
            "event_role": role,
            "fact_class": "invariant_state",
            "state_model_input": True,
            "dag_input": False,
            "model_input": True,
            "phase": "end",
            **args,
        },
    )


def observation_event(ts: int, role: str, fact_class: str, target_id: str, args: dict[str, Any]) -> dict[str, Any]:
    return hicache_event(
        ts,
        f"hicache_{role}_end",
        {
            "target_id": target_id,
            "event_role": role,
            "fact_class": fact_class,
            "state_model_input": False,
            "dag_input": fact_class == "timing_observation",
            "model_input": True,
            "phase": "end",
            **args,
        },
    )


def hicache_event(ts: int, name: str, args: dict[str, Any], pid: int = 1, tid: int = 1) -> dict[str, Any]:
    base_args: dict[str, Any] = {"domain": "python_probe", "event_kind": "hicache"}
    base_args.update(args)
    return {"name": name, "cat": "python_probe", "ph": "X", "ts": ts, "dur": 1, "pid": pid, "tid": tid, "args": base_args}


def token_dictionary(path_id: str, tokens: list[int]) -> dict[str, Any]:
    return {
        "token_path_id": path_id,
        "token_ids": tokens,
        "token_count": len(tokens),
        "hash_algo": HASH_ALGO,
    }


def token_span(path_id: str, begin: int, end: int) -> dict[str, Any]:
    return {
        "path_id": path_id,
        "begin": begin,
        "end": end,
        "token_count": end - begin,
        "hash_algo": HASH_ALGO,
    }


def page_ids(tokens: list[int], page_size: int, scope: str) -> list[str]:
    parent = b""
    pages: list[str] = []
    aligned_len = len(tokens) // page_size * page_size
    for begin in range(0, aligned_len, page_size):
        digest = hashlib.sha256()
        if parent:
            digest.update(parent)
        for token in tokens[begin : begin + page_size]:
            digest.update(int(token).to_bytes(4, "little", signed=False))
        parent = digest.digest()
        pages.append(f"{scope}|{parent.hex()}")
    return pages


def run_hicache_trace(tmp: Path, name: str, events: list[dict[str, Any]], hicache_config: dict[str, Any]) -> dict[str, Any]:
    trace_path = tmp / f"{name}_trace.json"
    model_config = tmp / f"{name}_model.json"
    summary_out = tmp / f"{name}_summary.json"
    run_summary = tmp / f"{name}_run.json"
    trace_path.write_text(json.dumps({"traceEvents": events}, ensure_ascii=True), encoding="utf-8")
    cfg = {"modules": ["hicache"], "hicache": {"enabled": True, **hicache_config}}
    model_config.write_text(json.dumps(cfg, ensure_ascii=True), encoding="utf-8")
    subprocess.check_call(
        [
            str(ROOT / "build/bin/trace_graph"),
            "--input",
            str(trace_path),
            "--run-summary",
            str(run_summary),
            "--model-config",
            str(model_config),
            "--model-summary",
            str(summary_out),
        ],
        cwd=ROOT,
    )
    summary = json.loads(summary_out.read_text(encoding="utf-8"))
    modules = summary["modules"]
    assert modules, summary
    return modules[0]["hicache"]


if __name__ == "__main__":
    raise SystemExit(main())
