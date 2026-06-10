#!/usr/bin/env python3
"""C++ HiCache state module fixtures。"""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    with tempfile.TemporaryDirectory() as raw_tmp:
        tmp = Path(raw_tmp)
        run_observed_policy_rejected_fixture(tmp)
        run_non_invariant_movement_skipped_fixture(tmp)
        run_write_back_capacity_fixture(tmp)
        run_lookup_touch_capacity_fixture(tmp)
        run_target_lookup_load_fixture(tmp)
        run_page_size_invariant_fixture(tmp)
        run_page_size_target_identity_by_size_fixture(tmp)
        run_page_size_prefetch_transfer_target_identity_fixture(tmp)
        run_page_size_prefetch_progress_operation_pages_non_invariant_fixture(tmp)
        run_page_size_prefetch_schedule_lookup_suffix_fixture(tmp)
        run_page_size_prefetch_schedule_best_effort_target_suffix_fixture(tmp)
        run_page_size_prefetch_transfer_completion_extends_schedule_fixture(tmp)
        run_page_size_prefetch_transfer_completed_tokens_target_pages_fixture(tmp)
        run_target_radix_prefix_fixture(tmp)
        run_write_through_selective_fixture(tmp)
        run_lock_ref_fixture(tmp)
        run_page_size_lock_ref_non_invariant_fixture(tmp)
        run_leaf_group_l2_clear_fixture(tmp)
        run_radix_removed_pages_fixture(tmp)
        run_radix_removed_pages_lookup_fixture(tmp)
        run_radix_removed_pages_page_size_mismatch_fixture(tmp)
        run_target_radix_removed_pages_page_size_mismatch_fixture(tmp)
        run_dirty_insert_overwrites_prefetch_backup_fixture(tmp)
        run_write_back_loaded_prefix_not_reinserted_fixture(tmp)
        run_prefetch_transfer_completed_tokens_fixture(tmp)
        run_prefetch_duplicate_transfer_ready_without_resident_fixture(tmp)
        run_capacity_target_skips_source_remove_fixture(tmp)
        run_same_page_capacity_leaf_group_fixture(tmp)
        run_prefetch_wait_complete_fixture(tmp)
        run_prefetch_wait_complete_suppressed_fixture(tmp)
        run_prefetch_timeout_progress_fixture(tmp)
        run_prefetch_timeout_terminal_empty_fixture(tmp)
        run_prefetch_timeout_finalize_fixture(tmp)
        run_prefetch_timeout_transfer_credit_fixture(tmp)
        run_prefetch_timeout_lock_ref_non_invariant_fixture(tmp)
        run_prefetch_wait_complete_transfer_credit_fixture(tmp)
        run_prefetch_wait_complete_lookup_l3_credit_fixture(tmp)
        run_prefetch_best_effort_progress_fixture(tmp)
        run_prefetch_best_effort_transfer_credit_fixture(tmp)
        run_prefetch_best_effort_lock_ref_non_invariant_fixture(tmp)
        run_prefetch_write_back_transfer_non_invariant_fixture(tmp)
        run_prefetch_write_back_capacity_transfer_credit_fixture(tmp)
        run_same_page_policy_evict_leaf_group_fixture(tmp)
    print("hicache state fixtures passed")
    return 0


def run_observed_policy_rejected_fixture(tmp: Path) -> None:
    """验证显式 observed policy 已非法，不能回落到旧兼容语义。"""

    trace_path = tmp / "hicache_observed_policy_rejected_trace.json"
    trace_path.write_text(json.dumps({"traceEvents": []}, ensure_ascii=False), encoding="utf-8")
    for index, hicache_config in enumerate(
        (
            {"enabled": True, "write_policy": "observed"},
            {"enabled": True, "prefetch_policy": "observed"},
            {"enabled": True, "storage_prefetch_policy": "observed"},
        )
    ):
        model_config = tmp / f"hicache_observed_policy_rejected_{index}.json"
        summary_out = tmp / f"hicache_observed_policy_rejected_{index}_summary.json"
        run_summary = tmp / f"hicache_observed_policy_rejected_{index}_run.json"
        model_config.write_text(json.dumps({"modules": ["hicache"], "hicache": hicache_config}, ensure_ascii=False), encoding="utf-8")
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
        )
        assert result.returncode != 0, result.stdout + result.stderr
        assert "observed" in result.stderr or "observed" in result.stdout, result.stdout + result.stderr


def run_non_invariant_movement_skipped_fixture(tmp: Path) -> None:
        trace_path = tmp / "hicache_trace.json"
        model_config = tmp / "hicache_model.json"
        graph_out = tmp / "graph.json"
        summary_out = tmp / "summary.json"
        run_summary = tmp / "run_summary.json"
        trace_path.write_text(
            json.dumps(
                {
                    "traceEvents": [
                        hicache_event(10, "hicache_insert_end", {"event_role": "insert", "tier_dst": "L1", "page_identity": '["p1","p2"]'}),
                        state_snapshot_event(15, {"derived": {"l1_resident_pages": ["debug_only"]}}),
                        hicache_event(
                            20,
                            "hicache_l2_l1_enqueue_end",
                            {"direction": "load", "tier_src": "L2", "tier_dst": "L1", "num_pages": 1, "bytes": 64, "page_identity": "p3"},
                        ),
                        hicache_event(
                            30,
                            "hicache_write_backup_end",
                            {"event_role": "write_backup", "tier_src": "L1", "tier_dst": "L2", "num_pages": 1, "page_identity": "p1"},
                        ),
                        hicache_event(
                            40,
                            "hicache_write_storage_schedule_end",
                            {"event_role": "write_storage_schedule", "tier_src": "L2", "tier_dst": "L3", "num_pages": 1, "page_identity": "p1"},
                        ),
                        hicache_event(50, "hicache_l3_prefetch_enqueue_end", {"event_role": "l3_prefetch_enqueue", "page_identity": "p4"}),
                        hicache_event(
                            60,
                            "hicache_l3_l2_transfer_end",
                            {"event_role": "l3_to_l2_transfer", "direction": "prefetch", "tier_src": "L3", "tier_dst": "L2", "page_identity": "p4"},
                        ),
                        hicache_event(70, "hicache_evict_end", {"event_role": "evict", "tier_src": "L1", "page_identity": "p2"}),
                    ]
                },
                ensure_ascii=False,
            ),
            encoding="utf-8",
        )
        model_config.write_text(
            json.dumps(
                {
                    "modules": ["hicache"],
                    "hicache": {"enabled": True},
                },
                ensure_ascii=False,
            ),
            encoding="utf-8",
        )
        subprocess.check_call(
            [
                str(ROOT / "build/bin/trace_graph"),
                "--input",
                str(trace_path),
                "--graph-output",
                str(graph_out),
                "--full-output",
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
        cache_summary = modules[0]["hicache"]
        assert cache_summary["status"] == "state_model", cache_summary
        assert cache_summary["input_hicache_events"] == 7, cache_summary
        assert cache_summary["processed_hicache_events"] == 7, cache_summary
        assert cache_summary["dag_mutations"] == 0, cache_summary
        assert cache_summary["state_transition_count"] > 0, cache_summary
        assert len(cache_summary["transition_trace"]) == cache_summary["state_transition_count"], cache_summary
        assert "before_state_digest" not in cache_summary["transition_trace"][0], cache_summary
        assert cache_summary["target_config"]["emit_state_digests"] is False, cache_summary
        assert cache_summary["dirty_eviction_events"] == 0, cache_summary
        assert cache_summary["skipped_non_invariant_events"] == 6, cache_summary
        assert cache_summary["non_invariant_fact_usage"] == [], cache_summary
        assert cache_summary["missing_page_identity_events"] == 0, cache_summary
        final_state = cache_summary["final_state"]
        assert final_state["l1_resident_pages"] == ["p1", "p2"], final_state
        assert final_state["l2_resident_pages"] == ["p1", "p2"], final_state
        assert final_state["l3_resident_pages"] == ["p1", "p2"], final_state
        assert final_state["dirty_pages"] == [], final_state
        assert final_state["backuped_pages"] == ["p1", "p2"], final_state
        assert final_state["evicted_pages"] == [], final_state
        assert final_state["prefetch_planned_pages"] == [], final_state
        assert final_state["prefetch_ready_pages"] == [], final_state
        assert cache_summary["transitions_by_kind"].get("mark_dirty", 0) == 0, cache_summary
        assert cache_summary["transitions_by_kind"].get("clear_dirty", 0) == 0, cache_summary
        assert cache_summary["transitions_by_kind"].get("mark_evicted", 0) == 0, cache_summary
        assert "pages_by_edge" not in cache_summary, cache_summary
        run = json.loads(run_summary.read_text(encoding="utf-8"))
        assert run["simulated_e2e_ns"] == run["real_e2e_ns"], run
        assert run["real_e2e_ns"] == 61, run


def run_write_back_capacity_fixture(tmp: Path) -> None:
    """验证 write-back 目标策略下，容量淘汰会触发 modeled writeback。"""

    trace_path = tmp / "hicache_write_back_capacity.json"
    model_config = tmp / "hicache_write_back_capacity_model.json"
    summary_out = tmp / "hicache_write_back_capacity_summary.json"
    run_summary = tmp / "hicache_write_back_capacity_run.json"
    trace_path.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(10, "hicache_insert_end", {"event_role": "insert", "tier_dst": "L1", "page_identity": "p1"}),
                    hicache_event(20, "hicache_insert_end", {"event_role": "insert", "tier_dst": "L1", "page_identity": "p2"}),
                    hicache_event(30, "hicache_insert_end", {"event_role": "insert", "tier_dst": "L1", "page_identity": "p3"}),
                    hicache_event(40, "hicache_write_backup_end", {"event_role": "write_backup", "tier_src": "L1", "tier_dst": "L2", "page_identity": "p3"}),
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    model_config.write_text(
        json.dumps(
            {
                "modules": ["hicache"],
                "hicache": {"enabled": True, "write_policy": "write_back", "l1_capacity_pages": 2},
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
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
    cache_summary = json.loads(summary_out.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    final_state = cache_summary["final_state"]
    assert final_state["l1_resident_pages"] == ["p2", "p3"], final_state
    assert final_state["dirty_pages"] == ["p2", "p3"], final_state
    assert final_state["backuped_pages"] == ["p1"], final_state
    assert final_state["evicted_pages"] == ["p1"], final_state
    assert cache_summary["dirty_eviction_events"] == 1, cache_summary
    assert cache_summary["target_config"]["write_policy"] == "write_back", cache_summary


def run_lookup_touch_capacity_fixture(tmp: Path) -> None:
    """验证 lookup hit 会刷新 LRU-like touch order。"""

    trace_path = tmp / "hicache_lookup_touch_capacity.json"
    model_config = tmp / "hicache_lookup_touch_capacity_model.json"
    summary_out = tmp / "hicache_lookup_touch_capacity_summary.json"
    run_summary = tmp / "hicache_lookup_touch_capacity_run.json"
    trace_path.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(10, "hicache_insert_end", {"event_role": "insert", "tier_dst": "L1", "page_identity": "p1"}),
                    hicache_event(20, "hicache_insert_end", {"event_role": "insert", "tier_dst": "L1", "page_identity": "p2"}),
                    hicache_event(30, "hicache_lookup_end", {"event_role": "lookup", "page_identity": "p1"}),
                    hicache_event(40, "hicache_insert_end", {"event_role": "insert", "tier_dst": "L1", "page_identity": "p3"}),
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    model_config.write_text(json.dumps({"modules": ["hicache"], "hicache": {"enabled": True, "l1_capacity_pages": 2}}, ensure_ascii=False), encoding="utf-8")
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(trace_path), "--run-summary", str(run_summary), "--model-config", str(model_config), "--model-summary", str(summary_out)],
        cwd=ROOT,
    )
    cache_summary = json.loads(summary_out.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    assert cache_summary["final_state"]["l1_resident_pages"] == ["p1", "p3"], cache_summary
    assert cache_summary["final_state"]["evicted_pages"] == ["p2"], cache_summary


def run_target_lookup_load_fixture(tmp: Path) -> None:
    """验证 page size what-if 下 lookup 能从 target L2 重新加载 L1。"""

    trace_path = tmp / "hicache_target_lookup_load.json"
    model_config = tmp / "hicache_target_lookup_load_model.json"
    summary_out = tmp / "hicache_target_lookup_load_summary.json"
    run_summary = tmp / "hicache_target_lookup_load_run.json"
    trace_path.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(10, "hicache_lookup_end", {"event_role": "lookup", "request_id": "req-a", "page_size": 128, "target_page_identity": ["p1"]}),
                    hicache_event(20, "hicache_insert_end", {"event_role": "insert", "request_id": "req-a", "page_size": 128, "target_page_identity": ["p1"]}),
                    hicache_event(30, "hicache_lookup_end", {"event_role": "lookup", "request_id": "req-b", "page_size": 128, "target_page_identity": ["p2"]}),
                    hicache_event(40, "hicache_insert_end", {"event_role": "insert", "request_id": "req-b", "page_size": 128, "target_page_identity": ["p2"]}),
                    hicache_event(50, "hicache_lookup_end", {"event_role": "lookup", "request_id": "req-a", "page_size": 128, "target_page_identity": ["p1"]}),
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    model_config.write_text(
        json.dumps({"modules": ["hicache"], "hicache": {"enabled": True, "page_size": 64, "l1_capacity_pages": 1, "l2_capacity_pages": 2, "write_policy": "write_through"}}, ensure_ascii=False),
        encoding="utf-8",
    )
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(trace_path), "--run-summary", str(run_summary), "--model-config", str(model_config), "--model-summary", str(summary_out)],
        cwd=ROOT,
    )
    cache_summary = json.loads(summary_out.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    assert cache_summary["final_state"]["l1_resident_pages"] == ["p1"], cache_summary
    assert cache_summary["final_state"]["l2_resident_pages"] == ["p1", "p2"], cache_summary
    assert cache_summary["final_state"]["l3_resident_pages"] == ["p1", "p2"], cache_summary


def run_page_size_invariant_fixture(tmp: Path) -> None:
    """验证跨 page size 预测必须使用 target page identity 或暴露缺失不变量。"""

    missing_trace = tmp / "hicache_page_size_missing.json"
    skipped_trace = tmp / "hicache_page_size_skipped_non_invariant.json"
    target_trace = tmp / "hicache_page_size_target.json"
    missing_summary = tmp / "hicache_page_size_missing_summary.json"
    skipped_summary = tmp / "hicache_page_size_skipped_non_invariant_summary.json"
    target_summary = tmp / "hicache_page_size_target_summary.json"
    model_config = tmp / "hicache_page_size_model.json"
    run_summary = tmp / "hicache_page_size_run.json"
    missing_trace.write_text(
        json.dumps(
            {"traceEvents": [hicache_event(10, "hicache_insert_end", {"event_role": "insert", "page_size": 128, "insert_tokens": 128, "page_identity": "base_p1"})]},
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    target_trace.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(
                        8,
                        "hicache_prefetch_schedule_end",
                        {
                            "event_role": "prefetch_schedule",
                            "request_id": "req-wait-suppressed",
                            "page_size": 128,
                            "page_identity": '["p1","p2"]',
                        },
                    ),
                    hicache_event(
                        10,
                        "hicache_insert_end",
                        {"event_role": "insert", "page_size": 128, "insert_tokens": 128, "page_identity": "base_p1", "target_page_identity": ["target_p1", "target_p2"]},
                    )
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    skipped_trace.write_text(
        json.dumps(
            {"traceEvents": [hicache_event(10, "hicache_load_back_end", {"event_role": "load_back", "page_size": 128, "page_identity": "base_p1"})]},
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    model_config.write_text(json.dumps({"modules": ["hicache"], "hicache": {"enabled": True, "page_size": 64}}, ensure_ascii=False), encoding="utf-8")
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(missing_trace), "--run-summary", str(run_summary), "--model-config", str(model_config), "--model-summary", str(missing_summary)],
        cwd=ROOT,
    )
    missing = json.loads(missing_summary.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    assert missing["missing_invariant_facts"]["target_page_identity_or_token_path"] == 1, missing
    assert missing["final_state"]["l1_resident_pages"] == [], missing
    subprocess.check_call(
        [
            str(ROOT / "build/bin/trace_graph"),
            "--input",
            str(skipped_trace),
            "--run-summary",
            str(run_summary),
            "--model-config",
            str(model_config),
            "--model-summary",
            str(skipped_summary),
        ],
        cwd=ROOT,
    )
    skipped = json.loads(skipped_summary.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    assert skipped["missing_invariant_facts"] == {}, skipped
    assert skipped["skipped_non_invariant_events"] == 1, skipped
    assert skipped["final_state"]["l1_resident_pages"] == [], skipped
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(target_trace), "--run-summary", str(run_summary), "--model-config", str(model_config), "--model-summary", str(target_summary)],
        cwd=ROOT,
    )
    target = json.loads(target_summary.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    assert target["missing_invariant_facts"] == {}, target
    assert target["final_state"]["l1_resident_pages"] == ["target_p1", "target_p2"], target


def run_page_size_target_identity_by_size_fixture(tmp: Path) -> None:
    """验证共享 profiling 可按目标 page size 选择对应 target identity 字段。"""

    trace_path = tmp / "hicache_page_size_target_identity_by_size.json"
    model_config = tmp / "hicache_page_size_target_identity_by_size_model.json"
    summary_out = tmp / "hicache_page_size_target_identity_by_size_summary.json"
    run_summary = tmp / "hicache_page_size_target_identity_by_size_run.json"
    trace_path.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(
                        10,
                        "hicache_insert_end",
                        {
                            "event_role": "insert",
                            "page_size": 128,
                            "page_identity": ["base128"],
                            "target_page_identity_page64": ["target64_a", "target64_b"],
                            "target_page_identity_page128": ["wrong128"],
                        },
                    )
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    model_config.write_text(
        json.dumps({"modules": ["hicache"], "hicache": {"enabled": True, "page_size": 64}}, ensure_ascii=False),
        encoding="utf-8",
    )
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(trace_path), "--run-summary", str(run_summary), "--model-config", str(model_config), "--model-summary", str(summary_out)],
        cwd=ROOT,
    )
    summary = json.loads(summary_out.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    assert summary["missing_invariant_facts"] == {}, summary
    assert summary["final_state"]["l1_resident_pages"] == ["target64_a", "target64_b"], summary


def run_page_size_prefetch_transfer_target_identity_fixture(tmp: Path) -> None:
    """验证 page size 变化时 L3->L2 transfer 可用 target_page_identity 作为 ready evidence。"""

    trace_path = tmp / "hicache_page_size_prefetch_transfer_target_identity.json"
    model_config = tmp / "hicache_page_size_prefetch_transfer_target_identity_model.json"
    summary_out = tmp / "hicache_page_size_prefetch_transfer_target_identity_summary.json"
    run_summary = tmp / "hicache_page_size_prefetch_transfer_target_identity_run.json"
    trace_path.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(
                        10,
                        "hicache_prefetch_schedule_end",
                        {
                            "event_role": "prefetch_schedule",
                            "request_id": "req-page64-prefetch",
                            "page_size": 128,
                            "page_identity": ["base_p1"],
                            "target_page_identity": ["target_p1", "target_p2"],
                        },
                    ),
                    hicache_event(
                        20,
                        "hicache_l3_l2_transfer_end",
                        {
                            "event_role": "l3_to_l2_transfer",
                            "request_id": "req-page64-prefetch",
                            "tier_src": "L3",
                            "tier_dst": "L2",
                            "page_size": 128,
                            "page_identity": ["base_p1"],
                            "target_page_identity": ["target_p1", "target_p2"],
                        },
                    ),
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    model_config.write_text(
        json.dumps(
            {
                "modules": ["hicache"],
                "hicache": {
                    "enabled": True,
                    "page_size": 64,
                    "prefetch_policy": "timeout",
                    "prefetch_timeout_base_sec": 10.0,
                    "prefetch_timeout_per_ki_token_sec": 0.0,
                    "prefetch_timeout_max_sec": 10.0,
                },
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(trace_path), "--run-summary", str(run_summary), "--model-config", str(model_config), "--model-summary", str(summary_out)],
        cwd=ROOT,
    )
    summary = json.loads(summary_out.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    assert summary["skipped_non_invariant_events"] == 0, summary
    assert summary["final_state"]["prefetch_planned_pages"] == ["target_p1", "target_p2"], summary
    assert summary["final_state"]["prefetch_ready_pages"] == ["target_p1", "target_p2"], summary
    assert summary["final_state"]["l2_resident_pages"] == ["target_p1", "target_p2"], summary


def run_page_size_prefetch_progress_operation_pages_non_invariant_fixture(tmp: Path) -> None:
    """验证跨 page size 时 progress operation pages 不会污染 target state。"""

    trace_path = tmp / "hicache_page_size_prefetch_progress_non_invariant.json"
    model_config = tmp / "hicache_page_size_prefetch_progress_non_invariant_model.json"
    summary_out = tmp / "hicache_page_size_prefetch_progress_non_invariant_summary.json"
    run_summary = tmp / "hicache_page_size_prefetch_progress_non_invariant_run.json"
    trace_path.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(
                        10,
                        "hicache_prefetch_schedule_end",
                        {
                            "event_role": "prefetch_schedule",
                            "request_id": "req-page64-progress",
                            "page_size": 128,
                            "page_identity": ["base_planned"],
                            "target_page_identity": ["target_p1", "target_p2"],
                        },
                    ),
                    hicache_event(
                        20,
                        "hicache_prefetch_progress_start",
                        {
                            "event_role": "prefetch_progress",
                            "request_id": "req-page64-progress",
                            "page_size": 128,
                            "prefetch_progress_state": {
                                "request_id": "req-page64-progress",
                                "page_size": 128,
                                "operation_hash_pages": ["base_operation_page"],
                                "completed_tokens": 128,
                                "check_return": None,
                                "has_ongoing_prefetch": True,
                            },
                        },
                    ),
                    hicache_event(
                        30,
                        "hicache_prefetch_progress_end",
                        {
                            "event_role": "prefetch_progress",
                            "request_id": "req-page64-progress",
                            "page_size": 128,
                            "prefetch_progress_state": {
                                "request_id": "req-page64-progress",
                                "page_size": 128,
                                "check_return": True,
                                "has_ongoing_prefetch": False,
                            },
                        },
                    ),
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    model_config.write_text(
        json.dumps(
            {
                "modules": ["hicache"],
                "hicache": {
                    "enabled": True,
                    "page_size": 64,
                    "prefetch_policy": "timeout",
                    "prefetch_timeout_base_sec": 10.0,
                    "prefetch_timeout_per_ki_token_sec": 0.0,
                    "prefetch_timeout_max_sec": 10.0,
                },
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(trace_path), "--run-summary", str(run_summary), "--model-config", str(model_config), "--model-summary", str(summary_out)],
        cwd=ROOT,
    )
    summary = json.loads(summary_out.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    final_state = summary["final_state"]
    assert summary["skipped_non_invariant_events"] == 0, summary
    assert final_state["prefetch_planned_pages"] == ["target_p1", "target_p2"], final_state
    assert final_state["prefetch_ready_pages"] == [], final_state
    assert final_state["prefetch_suppressed_pages"] == ["target_p1", "target_p2"], final_state
    assert "base_operation_page" not in final_state["l2_resident_pages"], final_state
    assert "base_operation_page" not in final_state["prefetch_ready_pages"], final_state
    assert final_state["l2_resident_pages"] == [], final_state


def run_page_size_prefetch_schedule_lookup_suffix_fixture(tmp: Path) -> None:
    """验证跨 page size 时 timeout prefetch schedule 可从 lookup target path 取 suffix。"""

    trace_path = tmp / "hicache_page_size_prefetch_schedule_lookup_suffix.json"
    model_config = tmp / "hicache_page_size_prefetch_schedule_lookup_suffix_model.json"
    summary_out = tmp / "hicache_page_size_prefetch_schedule_lookup_suffix_summary.json"
    run_summary = tmp / "hicache_page_size_prefetch_schedule_lookup_suffix_run.json"
    trace_path.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(
                        10,
                        "hicache_lookup_end",
                        {
                            "event_role": "lookup",
                            "request_id": "req-page64-schedule",
                            "page_size": 128,
                            "page_identity": ["base_prefix"],
                            "target_page_identity": ["target_prefix", "target_suffix"],
                        },
                    ),
                    hicache_event(
                        20,
                        "hicache_prefetch_schedule_end",
                        {
                            "event_role": "prefetch_schedule",
                            "request_id": "req-page64-schedule",
                            "page_size": 128,
                            "new_input_tokens": 97,
                            "page_identity": [],
                            "target_page_identity": ["wrong_no_parent_suffix"],
                        },
                    ),
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    model_config.write_text(
        json.dumps(
            {
                "modules": ["hicache"],
                "hicache": {
                    "enabled": True,
                    "page_size": 64,
                    "prefetch_policy": "timeout",
                },
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(trace_path), "--run-summary", str(run_summary), "--model-config", str(model_config), "--model-summary", str(summary_out)],
        cwd=ROOT,
    )
    summary = json.loads(summary_out.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    final_state = summary["final_state"]
    assert final_state["prefetch_planned_pages"] == ["target_suffix"], final_state
    assert "wrong_no_parent_suffix" not in final_state["prefetch_planned_pages"], final_state


def run_page_size_prefetch_schedule_best_effort_target_suffix_fixture(tmp: Path) -> None:
    """验证 best_effort page-size what-if 优先保留 prefetch target suffix。"""

    trace_path = tmp / "hicache_page_size_prefetch_schedule_best_effort_target_suffix.json"
    model_config = tmp / "hicache_page_size_prefetch_schedule_best_effort_target_suffix_model.json"
    summary_out = tmp / "hicache_page_size_prefetch_schedule_best_effort_target_suffix_summary.json"
    run_summary = tmp / "hicache_page_size_prefetch_schedule_best_effort_target_suffix_run.json"
    trace_path.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(
                        10,
                        "hicache_lookup_end",
                        {
                            "event_role": "lookup",
                            "request_id": "req-page64-best-schedule",
                            "page_size": 128,
                            "page_identity": ["base_prefix"],
                            "target_page_identity": ["target_prefix", "lookup_tail"],
                        },
                    ),
                    hicache_event(
                        20,
                        "hicache_prefetch_schedule_end",
                        {
                            "event_role": "prefetch_schedule",
                            "request_id": "req-page64-best-schedule",
                            "page_size": 128,
                            "new_input_tokens": 97,
                            "page_identity": [],
                            "target_page_identity": ["prefetch_parent_suffix"],
                        },
                    ),
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    model_config.write_text(
        json.dumps(
            {
                "modules": ["hicache"],
                "hicache": {
                    "enabled": True,
                    "page_size": 64,
                    "prefetch_policy": "best_effort",
                },
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(trace_path), "--run-summary", str(run_summary), "--model-config", str(model_config), "--model-summary", str(summary_out)],
        cwd=ROOT,
    )
    summary = json.loads(summary_out.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    final_state = summary["final_state"]
    assert final_state["prefetch_planned_pages"] == ["prefetch_parent_suffix"], final_state
    assert final_state["prefetch_suppressed_pages"] == ["prefetch_parent_suffix"], final_state
    assert "lookup_tail" not in final_state["prefetch_planned_pages"], final_state


def run_page_size_prefetch_transfer_completion_extends_schedule_fixture(tmp: Path) -> None:
    """验证跨 page size 时完整 base transfer 会补齐 target planned 尾页 ready。"""

    trace_path = tmp / "hicache_page_size_prefetch_transfer_extends_schedule.json"
    model_config = tmp / "hicache_page_size_prefetch_transfer_extends_schedule_model.json"
    summary_out = tmp / "hicache_page_size_prefetch_transfer_extends_schedule_summary.json"
    run_summary = tmp / "hicache_page_size_prefetch_transfer_extends_schedule_run.json"
    base_pages = [f"base_p{i:02d}" for i in range(1, 9)]
    target_pages = [f"target_p{i:02d}" for i in range(1, 18)]
    trace_path.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(
                        10,
                        "hicache_prefetch_schedule_end",
                        {
                            "event_role": "prefetch_schedule",
                            "request_id": "req-page64-transfer-tail",
                            "page_size": 128,
                            "new_input_tokens": 1121,
                            "page_identity": base_pages,
                            "target_page_identity": target_pages,
                        },
                    ),
                    hicache_event(
                        20,
                        "hicache_l3_l2_transfer_end",
                        {
                            "event_role": "l3_to_l2_transfer",
                            "request_id": "req-page64-transfer-tail",
                            "tier_src": "L3",
                            "tier_dst": "L2",
                            "page_size": 128,
                            "page_identity": base_pages,
                            "target_page_identity": target_pages[:-1],
                        },
                    ),
                    hicache_event(
                        30,
                        "hicache_prefetch_progress_end",
                        {
                            "event_role": "prefetch_progress",
                            "request_id": "req-page64-transfer-tail",
                            "page_size": 128,
                            "prefetch_progress_state": {
                                "request_id": "req-page64-transfer-tail",
                                "page_size": 128,
                                "check_return": True,
                                "has_ongoing_prefetch": False,
                            },
                        },
                    ),
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    model_config.write_text(
        json.dumps(
            {
                "modules": ["hicache"],
                "hicache": {
                    "enabled": True,
                    "page_size": 64,
                    "prefetch_policy": "timeout",
                    "prefetch_timeout_base_sec": 10.0,
                    "prefetch_timeout_per_ki_token_sec": 0.0,
                    "prefetch_timeout_max_sec": 10.0,
                },
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(trace_path), "--run-summary", str(run_summary), "--model-config", str(model_config), "--model-summary", str(summary_out)],
        cwd=ROOT,
    )
    summary = json.loads(summary_out.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    final_state = summary["final_state"]
    assert final_state["prefetch_planned_pages"] == target_pages, final_state
    assert final_state["prefetch_ready_pages"] == target_pages, final_state
    assert final_state["prefetch_suppressed_pages"] == [], final_state
    assert final_state["l2_resident_pages"] == target_pages, final_state

    equal_size_trace = tmp / "hicache_page_size_prefetch_transfer_equal_size_rekeys.json"
    equal_size_summary = tmp / "hicache_page_size_prefetch_transfer_equal_size_rekeys_summary.json"
    equal_size_trace.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(
                        10,
                        "hicache_prefetch_schedule_end",
                        {
                            "event_role": "prefetch_schedule",
                            "request_id": "req-page64-transfer-rekey",
                            "page_size": 128,
                            "page_identity": ["base_tail"],
                            "target_page_identity": ["target_tail_a", "target_tail_b"],
                        },
                    ),
                    hicache_event(
                        20,
                        "hicache_l3_l2_transfer_end",
                        {
                            "event_role": "l3_to_l2_transfer",
                            "request_id": "req-page64-transfer-rekey",
                            "tier_src": "L3",
                            "tier_dst": "L2",
                            "page_size": 128,
                            "page_identity": ["base_tail"],
                            "target_page_identity": ["wrong_tail_a", "wrong_tail_b"],
                        },
                    ),
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    subprocess.check_call(
        [
            str(ROOT / "build/bin/trace_graph"),
            "--input",
            str(equal_size_trace),
            "--run-summary",
            str(run_summary),
            "--model-config",
            str(model_config),
            "--model-summary",
            str(equal_size_summary),
        ],
        cwd=ROOT,
    )
    equal_size = json.loads(equal_size_summary.read_text(encoding="utf-8"))["modules"][0]["hicache"]["final_state"]
    assert equal_size["prefetch_planned_pages"] == ["target_tail_a", "target_tail_b"], equal_size
    assert equal_size["prefetch_ready_pages"] == ["target_tail_a", "target_tail_b"], equal_size
    assert equal_size["prefetch_suppressed_pages"] == [], equal_size
    assert equal_size["l2_resident_pages"] == ["target_tail_a", "target_tail_b"], equal_size
    assert "wrong_tail_a" not in equal_size["prefetch_ready_pages"], equal_size


def run_page_size_prefetch_transfer_completed_tokens_target_pages_fixture(tmp: Path) -> None:
    """验证 page-size what-if 下 transfer completed_tokens 按 target page size 给 ready credit。"""

    trace_path = tmp / "hicache_page_size_prefetch_transfer_completed_tokens_target_pages.json"
    model_config = tmp / "hicache_page_size_prefetch_transfer_completed_tokens_target_pages_model.json"
    summary_out = tmp / "hicache_page_size_prefetch_transfer_completed_tokens_target_pages_summary.json"
    run_summary = tmp / "hicache_page_size_prefetch_transfer_completed_tokens_target_pages_run.json"
    trace_path.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(
                        10,
                        "hicache_prefetch_schedule_end",
                        {
                            "event_role": "prefetch_schedule",
                            "request_id": "req-target-completed",
                            "page_size": 128,
                            "new_input_tokens": 256,
                            "page_identity": ["base_p1", "base_p2"],
                            "target_page_identity": ["target_p1", "target_p2", "target_p3", "target_p4"],
                        },
                    ),
                    hicache_event(
                        20,
                        "hicache_l3_l2_transfer_end",
                        {
                            "event_role": "l3_to_l2_transfer",
                            "request_id": "req-target-completed",
                            "direction": "prefetch",
                            "tier_src": "L3",
                            "tier_dst": "L2",
                            "page_size": 128,
                            "completed_tokens": 128,
                            "page_identity": ["base_p1", "base_p2"],
                            "target_page_identity": ["target_p1", "target_p2", "target_p3", "target_p4"],
                        },
                    ),
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    model_config.write_text(
        json.dumps(
            {"modules": ["hicache"], "hicache": {"enabled": True, "page_size": 64, "prefetch_policy": "best_effort"}},
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(trace_path), "--run-summary", str(run_summary), "--model-config", str(model_config), "--model-summary", str(summary_out)],
        cwd=ROOT,
    )
    summary = json.loads(summary_out.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    final_state = summary["final_state"]
    assert final_state["l2_resident_pages"] == ["target_p1", "target_p2"], final_state
    assert final_state["backuped_pages"] == ["target_p1", "target_p2"], final_state
    assert final_state["prefetch_ready_pages"] == ["target_p1", "target_p2"], final_state
    assert final_state["prefetch_suppressed_pages"] == ["target_p3", "target_p4"], final_state


def run_target_radix_prefix_fixture(tmp: Path) -> None:
    """验证 page size what-if 下 insert 使用 target lookup prefix，而不是 base insert prefix。"""

    trace_path = tmp / "hicache_target_radix_prefix.json"
    model_config = tmp / "hicache_target_radix_prefix_model.json"
    summary_out = tmp / "hicache_target_radix_prefix_summary.json"
    run_summary = tmp / "hicache_target_radix_prefix_run.json"
    trace_path.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(
                        8,
                        "hicache_prefetch_schedule_end",
                        {
                            "event_role": "prefetch_schedule",
                            "request_id": "req-wait-suppressed",
                            "page_size": 128,
                            "page_identity": '["p1","p2"]',
                        },
                    ),
                    hicache_event(
                        10,
                        "hicache_lookup_end",
                        {"event_role": "lookup", "request_id": "req-a", "page_size": 128, "page_identity": "base_a", "target_page_identity": ["p1", "p2", "p3"]},
                    ),
                    hicache_event(
                        20,
                        "hicache_insert_end",
                        {"event_role": "insert", "request_id": "req-a", "page_size": 128, "page_identity": "base_a", "target_page_identity": ["p1", "p2", "p3"]},
                    ),
                    hicache_event(
                        30,
                        "hicache_lookup_end",
                        {
                            "event_role": "lookup",
                            "request_id": "req-b",
                            "page_size": 128,
                            "page_identity": "base_b",
                            "target_page_identity": ["p1", "p2", "p3", "p4"],
                        },
                    ),
                    hicache_event(
                        40,
                        "hicache_insert_end",
                        {
                            "event_role": "insert",
                            "request_id": "req-b",
                            "page_size": 128,
                            "page_identity": "base_b",
                            "target_page_identity": ["p1", "p2", "p3", "p4"],
                        },
                    ),
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    model_config.write_text(json.dumps({"modules": ["hicache"], "hicache": {"enabled": True, "page_size": 64}}, ensure_ascii=False), encoding="utf-8")
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(trace_path), "--run-summary", str(run_summary), "--model-config", str(model_config), "--model-summary", str(summary_out)],
        cwd=ROOT,
    )
    summary = json.loads(summary_out.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    assert summary["final_state"]["l1_resident_pages"] == ["p1", "p2", "p3", "p4"], summary
    assert summary["transitions_by_kind"]["add_l1_resident"] == 4, summary


def run_write_through_selective_fixture(tmp: Path) -> None:
    """验证 selective 写策略按 hit_count threshold 触发 backup。

    SGLang 的 `write_through_selective` 默认阈值为 2。第一次 insert 只让
    page 留在 L1/dirty；第二次相同 path 的 insert 命中已有 node 后才
    write_backup。第三次命中只增加 hit_count，不应重复产生 backup 状态变更。
    """

    trace_path = tmp / "hicache_write_through_selective.json"
    model_config = tmp / "hicache_write_through_selective_model.json"
    summary_out = tmp / "hicache_write_through_selective_summary.json"
    run_summary = tmp / "hicache_write_through_selective_run.json"
    trace_path.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(
                        10,
                        "hicache_lookup_end",
                        {"event_role": "lookup", "request_id": "req-a", "page_size": 64, "page_identity": ["p1", "p2"]},
                    ),
                    hicache_event(
                        20,
                        "hicache_insert_end",
                        {"event_role": "insert", "request_id": "req-a", "page_size": 64, "page_identity": ["p1", "p2"]},
                    ),
                    hicache_event(
                        30,
                        "hicache_lookup_end",
                        {"event_role": "lookup", "request_id": "req-b", "page_size": 64, "page_identity": ["p1", "p2"]},
                    ),
                    hicache_event(
                        40,
                        "hicache_insert_end",
                        {"event_role": "insert", "request_id": "req-b", "page_size": 64, "page_identity": ["p1", "p2"], "prefix_len": 128},
                    ),
                    hicache_event(
                        50,
                        "hicache_lookup_end",
                        {"event_role": "lookup", "request_id": "req-c", "page_size": 64, "page_identity": ["p1", "p2"]},
                    ),
                    hicache_event(
                        60,
                        "hicache_insert_end",
                        {"event_role": "insert", "request_id": "req-c", "page_size": 64, "page_identity": ["p1", "p2"], "prefix_len": 128},
                    ),
                    hicache_event(
                        70,
                        "hicache_lookup_end",
                        {"event_role": "lookup", "request_id": "req-d", "page_size": 64, "page_identity": ["p3"]},
                    ),
                    hicache_event(
                        80,
                        "hicache_insert_end",
                        {"event_role": "insert", "request_id": "req-d", "page_size": 64, "page_identity": ["p3"]},
                    ),
                    hicache_event(
                        90,
                        "hicache_lookup_end",
                        {"event_role": "lookup", "request_id": "req-e", "page_size": 64, "page_identity": ["p4"]},
                        pid=1,
                    ),
                    hicache_event(
                        100,
                        "hicache_insert_end",
                        {"event_role": "insert", "request_id": "req-e", "page_size": 64, "page_identity": ["p4"]},
                        pid=1,
                    ),
                    hicache_event(
                        110,
                        "hicache_lookup_end",
                        {"event_role": "lookup", "request_id": "req-e", "page_size": 64, "page_identity": ["p4"]},
                        pid=2,
                    ),
                    hicache_event(
                        120,
                        "hicache_insert_end",
                        {"event_role": "insert", "request_id": "req-e", "page_size": 64, "page_identity": ["p4"]},
                        pid=2,
                    ),
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    model_config.write_text(
        json.dumps({"modules": ["hicache"], "hicache": {"enabled": True, "page_size": 64, "write_policy": "write_through_selective"}}, ensure_ascii=False),
        encoding="utf-8",
    )
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(trace_path), "--run-summary", str(run_summary), "--model-config", str(model_config), "--model-summary", str(summary_out)],
        cwd=ROOT,
    )
    summary = json.loads(summary_out.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    final_state = summary["final_state"]
    assert final_state["l1_resident_pages"] == ["p1", "p2", "p3", "p4"], final_state
    assert final_state["l2_resident_pages"] == ["p1", "p2"], final_state
    assert final_state["l3_resident_pages"] == ["p1", "p2"], final_state
    assert final_state["backuped_pages"] == ["p1", "p2"], final_state
    assert final_state["dirty_pages"] == ["p3", "p4"], final_state
    assert final_state["page_hit_counts"] == {"p1": 3, "p2": 3, "p3": 1, "p4": 1}, final_state
    assert summary["transitions_by_kind"]["increment_hit_count"] == 9, summary
    assert summary["transitions_by_kind"]["add_l2_resident"] == 2, summary
    assert summary["transitions_by_kind"]["add_l3_resident"] == 2, summary
    assert summary["transitions_by_kind"]["clear_dirty"] == 2, summary


def run_lock_ref_fixture(tmp: Path) -> None:
    """验证 lock/ref facts 只作为非不变量计数，不直接驱动 target state。"""

    trace_path = tmp / "hicache_lock_ref.json"
    model_config = tmp / "hicache_lock_ref_model.json"
    summary_out = tmp / "hicache_lock_ref_summary.json"
    run_summary = tmp / "hicache_lock_ref_run.json"
    trace_path.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(5, "hicache_inc_lock_ref_end", {"event_role": "lock_ref_inc", "page_identity": [], "lock_delta": 0}, pid=1),
                    hicache_event(10, "hicache_inc_lock_ref_end", {"event_role": "lock_ref_inc", "page_identity": ["p1", "p2"]}, pid=1),
                    hicache_event(20, "hicache_inc_lock_ref_end", {"event_role": "lock_ref_inc", "page_identity": ["p1"]}, pid=1),
                    hicache_event(25, "hicache_inc_lock_ref_end", {"event_role": "lock_ref_inc", "page_identity": ["p1"]}, pid=2),
                    hicache_event(30, "hicache_dec_lock_ref_end", {"event_role": "lock_ref_dec", "page_identity": ["p1"]}, pid=1),
                    hicache_event(40, "hicache_dec_lock_ref_end", {"event_role": "lock_ref_dec", "page_identity": ["p1"]}, pid=1),
                    hicache_event(50, "hicache_dec_lock_ref_end", {"event_role": "lock_ref_dec", "page_identity": ["p1"]}, pid=2),
                    hicache_event(55, "hicache_dec_lock_ref_end", {"event_role": "lock_ref_dec", "page_identity": [], "lock_delta": 0}, pid=1),
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    model_config.write_text(json.dumps({"modules": ["hicache"], "hicache": {"enabled": True}}, ensure_ascii=False), encoding="utf-8")
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(trace_path), "--run-summary", str(run_summary), "--model-config", str(model_config), "--model-summary", str(summary_out)],
        cwd=ROOT,
    )
    summary = json.loads(summary_out.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    final_state = summary["final_state"]
    assert final_state["locked_pages"] == [], final_state
    assert summary["skipped_non_invariant_events"] == 8, summary
    assert summary["lock_state_events"] == 0, summary
    assert summary["missing_page_identity_events"] == 0, summary
    assert "mark_locked" not in summary["transitions_by_kind"], summary
    assert "clear_locked" not in summary["transitions_by_kind"], summary


def run_page_size_lock_ref_non_invariant_fixture(tmp: Path) -> None:
    """验证 page size what-if 不把 base lock/ref 震荡当作 target 不变量。"""

    trace_path = tmp / "hicache_page_size_lock_ref_non_invariant.json"
    model_config = tmp / "hicache_page_size_lock_ref_non_invariant_model.json"
    summary_out = tmp / "hicache_page_size_lock_ref_non_invariant_summary.json"
    run_summary = tmp / "hicache_page_size_lock_ref_non_invariant_run.json"
    trace_path.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(
                        10,
                        "hicache_inc_lock_ref_end",
                        {
                            "event_role": "lock_ref_inc",
                            "page_size": 128,
                            "page_identity": "base-p1",
                            "target_page_identity": ["target-p1", "target-p2"],
                        },
                    ),
                    hicache_event(
                        20,
                        "hicache_dec_lock_ref_end",
                        {
                            "event_role": "lock_ref_dec",
                            "page_size": 128,
                            "page_identity": "base-p1",
                            "target_page_identity": ["target-p1", "target-p2"],
                        },
                    ),
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    model_config.write_text(
        json.dumps({"modules": ["hicache"], "hicache": {"enabled": True, "page_size": 64}}, ensure_ascii=False),
        encoding="utf-8",
    )
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(trace_path), "--run-summary", str(run_summary), "--model-config", str(model_config), "--model-summary", str(summary_out)],
        cwd=ROOT,
    )
    summary = json.loads(summary_out.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    assert summary["processed_events_by_role"]["lock_ref_inc"] == 1, summary
    assert summary["processed_events_by_role"]["lock_ref_dec"] == 1, summary
    assert summary["skipped_non_invariant_events"] == 2, summary
    assert summary["lock_state_events"] == 0, summary
    assert summary["final_state"]["locked_pages"] == [], summary
    assert "mark_locked" not in summary["transitions_by_kind"], summary
    assert "clear_locked" not in summary["transitions_by_kind"], summary


def run_leaf_group_l2_clear_fixture(tmp: Path) -> None:
    """验证 target leaf group 整体淘汰，以及 L2 删除会清理 evicted 状态。"""

    trace_path = tmp / "hicache_leaf_group_l2_clear.json"
    model_config = tmp / "hicache_leaf_group_l2_clear_model.json"
    summary_out = tmp / "hicache_leaf_group_l2_clear_summary.json"
    run_summary = tmp / "hicache_leaf_group_l2_clear_run.json"
    trace_path.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(
                        10,
                        "hicache_insert_end",
                        {
                            "event_role": "insert",
                            "page_size": 128,
                            "page_identity": "base_a",
                            "target_page_identity": ["p1", "p2", "p3"],
                        },
                    ),
                    hicache_event(
                        20,
                        "hicache_insert_end",
                        {
                            "event_role": "insert",
                            "page_size": 128,
                            "page_identity": "base_b",
                            "target_page_identity": ["p4"],
                        },
                    ),
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    model_config.write_text(
        json.dumps(
            {
                "modules": ["hicache"],
                "hicache": {
                    "enabled": True,
                    "page_size": 64,
                    "l1_capacity_pages": 3,
                    "l2_capacity_pages": 3,
                    "write_policy": "write_through",
                },
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(trace_path), "--run-summary", str(run_summary), "--model-config", str(model_config), "--model-summary", str(summary_out)],
        cwd=ROOT,
    )
    summary = json.loads(summary_out.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    assert summary["final_state"]["l1_resident_pages"] == ["p4"], summary
    assert summary["final_state"]["l2_resident_pages"] == ["p4"], summary
    assert summary["final_state"]["backuped_pages"] == ["p4"], summary
    assert summary["final_state"]["evicted_pages"] == [], summary
    assert summary["transitions_by_kind"]["remove_l1_resident"] == 3, summary
    assert summary["transitions_by_kind"]["remove_l2_resident"] == 3, summary
    assert summary["transitions_by_kind"]["clear_evicted"] == 3, summary


def run_radix_removed_pages_fixture(tmp: Path) -> None:
    """验证 insert 期间消失的 radix leaf 会清理 L2/backuped/evicted 状态。"""

    trace_path = tmp / "hicache_radix_removed_pages.json"
    model_config = tmp / "hicache_radix_removed_pages_model.json"
    summary_out = tmp / "hicache_radix_removed_pages_summary.json"
    run_summary = tmp / "hicache_radix_removed_pages_run.json"
    trace_path.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(10, "hicache_insert_end", {"event_role": "insert", "tier_dst": "L1", "page_size": 128, "page_identity": ["old_a", "old_b"]}),
                    hicache_event(
                        20,
                        "hicache_write_backup_end",
                        {"event_role": "write_backup", "tier_src": "L1", "tier_dst": "L2", "page_size": 128, "page_identity": ["old_a", "old_b"]},
                    ),
                    hicache_event(
                        30,
                        "hicache_remove_page_end",
                        {"event_role": "remove_page", "tier_src": "GPU", "page_size": 128, "page_identity": ["old_a", "old_b"]},
                    ),
                    hicache_event(
                        40,
                        "hicache_insert_end",
                        {
                            "event_role": "insert",
                            "tier_dst": "L1",
                            "page_size": 128,
                            "page_identity": ["new_a"],
                            "radix_removed_page_identity": ["old_a", "old_b"],
                        },
                    ),
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    model_config.write_text(
        json.dumps(
            {
                "modules": ["hicache"],
                "hicache": {
                    "enabled": True,
                    "page_size": 128,
                    "write_policy": "write_through",
                },
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(trace_path), "--run-summary", str(run_summary), "--model-config", str(model_config), "--model-summary", str(summary_out)],
        cwd=ROOT,
    )
    summary = json.loads(summary_out.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    assert summary["skipped_non_invariant_events"] == 2, summary
    assert summary["final_state"]["l1_resident_pages"] == ["new_a"], summary
    assert summary["final_state"]["l2_resident_pages"] == ["new_a"], summary
    assert summary["final_state"]["l3_resident_pages"] == ["new_a", "old_a", "old_b"], summary
    assert summary["final_state"]["backuped_pages"] == ["new_a"], summary
    assert summary["final_state"]["evicted_pages"] == [], summary
    assert summary["transitions_by_kind"]["remove_l2_resident"] == 2, summary
    assert summary["transitions_by_kind"]["clear_backuped"] == 2, summary
    assert summary["transitions_by_kind"].get("clear_evicted", 0) == 0, summary


def run_radix_removed_pages_lookup_fixture(tmp: Path) -> None:
    """验证非 insert 调用物化的 radix removed pages 也会清理状态。"""

    trace_path = tmp / "hicache_radix_removed_pages_lookup.json"
    model_config = tmp / "hicache_radix_removed_pages_lookup_model.json"
    summary_out = tmp / "hicache_radix_removed_pages_lookup_summary.json"
    run_summary = tmp / "hicache_radix_removed_pages_lookup_run.json"
    trace_path.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(10, "hicache_insert_end", {"event_role": "insert", "tier_dst": "L1", "page_size": 128, "page_identity": ["old_a"]}),
                    hicache_event(
                        20,
                        "hicache_write_backup_end",
                        {"event_role": "write_backup", "tier_src": "L1", "tier_dst": "L2", "page_size": 128, "page_identity": ["old_a"]},
                    ),
                    hicache_event(30, "hicache_remove_page_end", {"event_role": "remove_page", "tier_src": "GPU", "page_size": 128, "page_identity": ["old_a"]}),
                    hicache_event(
                        40,
                        "hicache_lookup_end",
                        {
                            "event_role": "lookup",
                            "page_size": 128,
                            "page_identity": ["new_lookup"],
                            "radix_removed_page_identity": ["old_a"],
                        },
                    ),
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    model_config.write_text(
        json.dumps({"modules": ["hicache"], "hicache": {"enabled": True, "page_size": 128, "write_policy": "write_through"}}, ensure_ascii=False),
        encoding="utf-8",
    )
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
    summary = json.loads(summary_out.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    assert summary["skipped_non_invariant_events"] == 2, summary
    assert summary["final_state"]["l1_resident_pages"] == [], summary
    assert summary["final_state"]["l2_resident_pages"] == [], summary
    assert summary["final_state"]["l3_resident_pages"] == ["old_a"], summary
    assert summary["final_state"]["backuped_pages"] == [], summary
    assert summary["final_state"]["evicted_pages"] == [], summary
    assert summary["transitions_by_kind"]["remove_l2_resident"] == 1, summary
    assert summary["transitions_by_kind"]["clear_backuped"] == 1, summary
    assert summary["transitions_by_kind"].get("clear_evicted", 0) == 0, summary


def run_radix_removed_pages_page_size_mismatch_fixture(tmp: Path) -> None:
    """验证 radix removed pages 在 page-size what-if 下不作为 target 结构事实消费。"""

    trace_path = tmp / "hicache_radix_removed_pages_page_size_mismatch.json"
    model_config = tmp / "hicache_radix_removed_pages_page_size_mismatch_model.json"
    summary_out = tmp / "hicache_radix_removed_pages_page_size_mismatch_summary.json"
    run_summary = tmp / "hicache_radix_removed_pages_page_size_mismatch_run.json"
    trace_path.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(10, "hicache_insert_end", {"event_role": "insert", "tier_dst": "L1", "page_size": 64, "page_identity": ["old_a"]}),
                    hicache_event(
                        20,
                        "hicache_write_backup_end",
                        {"event_role": "write_backup", "tier_src": "L1", "tier_dst": "L2", "page_size": 64, "page_identity": ["old_a"]},
                    ),
                    hicache_event(30, "hicache_remove_page_end", {"event_role": "remove_page", "tier_src": "GPU", "page_size": 64, "page_identity": ["old_a"]}),
                    hicache_event(
                        40,
                        "hicache_insert_end",
                        {
                            "event_role": "insert",
                            "tier_dst": "L1",
                            "page_size": 128,
                            "page_identity": ["base_new"],
                            "target_page_identity": ["target_new"],
                            "radix_removed_page_identity": ["old_a"],
                        },
                    ),
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    model_config.write_text(
        json.dumps(
            {
                "modules": ["hicache"],
                "hicache": {
                    "enabled": True,
                    "page_size": 64,
                    "write_policy": "write_through",
                },
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(trace_path), "--run-summary", str(run_summary), "--model-config", str(model_config), "--model-summary", str(summary_out)],
        cwd=ROOT,
    )
    summary = json.loads(summary_out.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    assert summary["skipped_non_invariant_events"] == 3, summary
    assert summary["final_state"]["l1_resident_pages"] == ["old_a", "target_new"], summary
    assert summary["final_state"]["l2_resident_pages"] == ["old_a", "target_new"], summary
    assert summary["final_state"]["l3_resident_pages"] == ["old_a", "target_new"], summary
    assert summary["final_state"]["backuped_pages"] == ["old_a", "target_new"], summary
    assert summary["final_state"]["evicted_pages"] == [], summary
    assert summary["transitions_by_kind"].get("remove_l2_resident", 0) == 0, summary
    assert summary["transitions_by_kind"].get("clear_backuped", 0) == 0, summary
    assert summary["transitions_by_kind"].get("clear_evicted", 0) == 0, summary


def run_target_radix_removed_pages_page_size_mismatch_fixture(tmp: Path) -> None:
    """验证 page-size what-if 下优先消费 target radix removed pages。"""

    trace_path = tmp / "hicache_target_radix_removed_pages_page_size_mismatch.json"
    model_config = tmp / "hicache_target_radix_removed_pages_page_size_mismatch_model.json"
    summary_out = tmp / "hicache_target_radix_removed_pages_page_size_mismatch_summary.json"
    run_summary = tmp / "hicache_target_radix_removed_pages_page_size_mismatch_run.json"
    trace_path.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(10, "hicache_insert_end", {"event_role": "insert", "tier_dst": "L1", "page_size": 64, "page_identity": ["target_old"]}),
                    hicache_event(
                        20,
                        "hicache_write_backup_end",
                        {"event_role": "write_backup", "tier_src": "L1", "tier_dst": "L2", "page_size": 64, "page_identity": ["target_old"]},
                    ),
                    hicache_event(30, "hicache_remove_page_end", {"event_role": "remove_page", "tier_src": "GPU", "page_size": 64, "page_identity": ["target_old"]}),
                    hicache_event(
                        40,
                        "hicache_insert_end",
                        {
                            "event_role": "insert",
                            "tier_dst": "L1",
                            "page_size": 128,
                            "page_identity": ["base_new"],
                            "target_page_identity": ["target_new"],
                            "radix_removed_page_identity": ["base_old"],
                            "target_radix_removed_page_identity": ["target_old"],
                        },
                    ),
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    model_config.write_text(
        json.dumps(
            {
                "modules": ["hicache"],
                "hicache": {
                    "enabled": True,
                    "page_size": 64,
                    "write_policy": "write_through",
                },
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(trace_path), "--run-summary", str(run_summary), "--model-config", str(model_config), "--model-summary", str(summary_out)],
        cwd=ROOT,
    )
    summary = json.loads(summary_out.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    assert summary["skipped_non_invariant_events"] == 2, summary
    assert summary["final_state"]["l1_resident_pages"] == ["target_new"], summary
    assert summary["final_state"]["l2_resident_pages"] == ["target_new"], summary
    assert summary["final_state"]["l3_resident_pages"] == ["target_new", "target_old"], summary
    assert summary["final_state"]["backuped_pages"] == ["target_new"], summary
    assert summary["final_state"]["evicted_pages"] == [], summary
    assert summary["transitions_by_kind"]["remove_l2_resident"] == 1, summary
    assert summary["transitions_by_kind"]["clear_backuped"] == 1, summary
    assert summary["transitions_by_kind"].get("clear_evicted", 0) == 0, summary


def run_dirty_insert_overwrites_prefetch_backup_fixture(tmp: Path) -> None:
    """验证 dirty insert 会覆盖 target prefetch 形成的同 page host backup。"""

    trace_path = tmp / "hicache_dirty_insert_overwrites_prefetch_backup.json"
    model_config = tmp / "hicache_dirty_insert_overwrites_prefetch_backup_model.json"
    summary_out = tmp / "hicache_dirty_insert_overwrites_prefetch_backup_summary.json"
    run_summary = tmp / "hicache_dirty_insert_overwrites_prefetch_backup_run.json"
    trace_path.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(
                        10,
                        "hicache_prefetch_schedule_end",
                        {"event_role": "prefetch_schedule", "request_id": "req-dirty", "page_size": 64, "new_input_tokens": 64, "page_identity": ["p1"]},
                    ),
                    hicache_event(
                        20,
                        "hicache_l3_l2_transfer_end",
                        {"event_role": "l3_to_l2_transfer", "request_id": "req-dirty", "tier_src": "L3", "tier_dst": "L2", "page_size": 64, "page_identity": ["p1"]},
                    ),
                    hicache_event(30, "hicache_insert_end", {"event_role": "insert", "tier_dst": "L1", "page_size": 64, "page_identity": ["p1"]}),
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    model_config.write_text(
        json.dumps(
            {
                "modules": ["hicache"],
                "hicache": {
                    "enabled": True,
                    "page_size": 64,
                    "write_policy": "write_back",
                    "prefetch_policy": "timeout",
                    "write_back_prefetch_transfer_credit": True,
                },
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(trace_path), "--run-summary", str(run_summary), "--model-config", str(model_config), "--model-summary", str(summary_out)],
        cwd=ROOT,
    )
    summary = json.loads(summary_out.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    assert summary["skipped_non_invariant_events"] == 0, summary
    assert summary["final_state"]["l1_resident_pages"] == ["p1"], summary
    assert summary["final_state"]["l2_resident_pages"] == [], summary
    assert summary["final_state"]["l3_resident_pages"] == ["p1"], summary
    assert summary["final_state"]["backuped_pages"] == [], summary
    assert summary["final_state"]["dirty_pages"] == ["p1"], summary
    assert summary["final_state"]["prefetch_ready_pages"] == ["p1"], summary
    assert summary["transitions_by_kind"]["remove_l2_resident"] == 1, summary
    assert summary["transitions_by_kind"]["clear_backuped"] == 1, summary
    assert summary["transitions_by_kind"]["mark_dirty"] == 1, summary


def run_write_back_loaded_prefix_not_reinserted_fixture(tmp: Path) -> None:
    """验证 page-size what-if 下已从 host load 的 target prefix 不会被 insert 重新置脏。"""

    trace_path = tmp / "hicache_write_back_loaded_prefix_not_reinserted.json"
    model_config = tmp / "hicache_write_back_loaded_prefix_not_reinserted_model.json"
    summary_out = tmp / "hicache_write_back_loaded_prefix_not_reinserted_summary.json"
    run_summary = tmp / "hicache_write_back_loaded_prefix_not_reinserted_run.json"
    trace_path.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(
                        5,
                        "hicache_prefetch_schedule_end",
                        {
                            "event_role": "prefetch_schedule",
                            "request_id": "req-a",
                            "page_size": 128,
                            "new_input_tokens": 64,
                            "page_identity": ["base_p1"],
                            "target_page_identity": ["target_p1"],
                        },
                    ),
                    hicache_event(
                        10,
                        "hicache_l3_l2_transfer_end",
                        {
                            "event_role": "l3_to_l2_transfer",
                            "request_id": "req-a",
                            "direction": "prefetch",
                            "tier_src": "L3",
                            "tier_dst": "L2",
                            "page_size": 128,
                            "page_identity": ["base_p1"],
                            "target_page_identity": ["target_p1"],
                        },
                    ),
                    hicache_event(
                        20,
                        "hicache_lookup_end",
                        {
                            "event_role": "lookup",
                            "request_id": "req-a",
                            "page_size": 128,
                            "page_identity": ["base_p1"],
                            "target_page_identity": ["target_p1"],
                        },
                    ),
                    hicache_event(
                        30,
                        "hicache_insert_end",
                        {
                            "event_role": "insert",
                            "request_id": "req-a",
                            "tier_dst": "L1",
                            "page_size": 128,
                            "page_identity": ["base_p1"],
                            "target_page_identity": ["target_p1"],
                        },
                    ),
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    model_config.write_text(
        json.dumps(
            {"modules": ["hicache"], "hicache": {"enabled": True, "page_size": 64, "write_policy": "write_back", "prefetch_policy": "best_effort"}},
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(trace_path), "--run-summary", str(run_summary), "--model-config", str(model_config), "--model-summary", str(summary_out)],
        cwd=ROOT,
    )
    summary = json.loads(summary_out.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    final_state = summary["final_state"]
    assert final_state["l1_resident_pages"] == ["target_p1"], final_state
    assert final_state["l2_resident_pages"] == ["target_p1"], final_state
    assert final_state["backuped_pages"] == ["target_p1"], final_state
    assert final_state["dirty_pages"] == [], final_state
    assert summary["transitions_by_kind"].get("remove_l2_resident", 0) == 0, summary
    assert summary["transitions_by_kind"].get("clear_backuped", 0) == 0, summary
    assert summary["transitions_by_kind"].get("mark_dirty", 0) == 0, summary


def run_prefetch_transfer_completed_tokens_fixture(tmp: Path) -> None:
    """验证 transfer completed_tokens 限制 L2/ready credit，尾页由 progress 收尾。"""

    trace_path = tmp / "hicache_prefetch_transfer_completed_tokens.json"
    model_config = tmp / "hicache_prefetch_transfer_completed_tokens_model.json"
    summary_out = tmp / "hicache_prefetch_transfer_completed_tokens_summary.json"
    run_summary = tmp / "hicache_prefetch_transfer_completed_tokens_run.json"
    pages = ["p1", "p2", "p3"]
    progress_start = {
        "request_id": "req-a",
        "policy": "best_effort",
        "page_size": 64,
        "operation_hash_pages": pages,
        "completed_tokens": 0,
        "ready_pages_estimate": 0,
        "has_ongoing_prefetch": True,
        "check_return": None,
    }
    progress_end = {
        "request_id": "req-a",
        "policy": "best_effort",
        "page_size": 64,
        "loaded_tokens_evidence": 128,
        "has_ongoing_prefetch": False,
        "check_return": True,
    }
    trace_path.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(
                        10,
                        "hicache_prefetch_schedule_end",
                        {"event_role": "prefetch_schedule", "request_id": "req-a", "page_size": 64, "new_input_tokens": 192, "page_identity": pages},
                    ),
                    hicache_event(
                        20,
                        "hicache_l3_l2_transfer_end",
                        {
                            "event_role": "l3_to_l2_transfer",
                            "direction": "prefetch",
                            "tier_src": "L3",
                            "tier_dst": "L2",
                            "request_id": "req-a",
                            "page_size": 64,
                            "completed_tokens": 128,
                            "page_identity": pages,
                        },
                    ),
                    hicache_event(
                        30,
                        "hicache_prefetch_progress_start",
                        {
                            "event_role": "prefetch_progress",
                            "request_id": "req-a",
                            "page_size": 64,
                            "prefetch_progress_state": progress_start,
                        },
                    ),
                    hicache_event(
                        40,
                        "hicache_prefetch_progress_end",
                        {
                            "event_role": "prefetch_progress",
                            "request_id": "req-a",
                            "prefetch_done": True,
                            "page_size": 64,
                            "prefetch_progress_state": progress_end,
                        },
                    ),
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    model_config.write_text(
        json.dumps({"modules": ["hicache"], "hicache": {"enabled": True, "page_size": 64, "prefetch_policy": "timeout"}}, ensure_ascii=False),
        encoding="utf-8",
    )
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(trace_path), "--run-summary", str(run_summary), "--model-config", str(model_config), "--model-summary", str(summary_out)],
        cwd=ROOT,
    )
    summary = json.loads(summary_out.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    assert summary["final_state"]["l2_resident_pages"] == ["p1", "p2"], summary
    assert summary["final_state"]["backuped_pages"] == ["p1", "p2"], summary
    assert summary["final_state"]["prefetch_ready_pages"] == ["p1", "p2"], summary
    assert summary["final_state"]["prefetch_late_pages"] == ["p3"], summary
    assert summary["final_state"]["prefetch_suppressed_pages"] == ["p3"], summary
    assert summary["transitions_by_kind"]["add_l2_resident"] == 2, summary
    assert summary["transitions_by_kind"]["mark_prefetch_ready"] == 2, summary
    assert summary["transitions_by_kind"]["mark_prefetch_late"] == 1, summary


def run_prefetch_duplicate_transfer_ready_without_resident_fixture(tmp: Path) -> None:
    """验证同 request 重复 transfer 只补 ready，不重复扩大全局 L2 resident。"""

    trace_path = tmp / "hicache_prefetch_duplicate_transfer_ready_without_resident.json"
    model_config = tmp / "hicache_prefetch_duplicate_transfer_ready_without_resident_model.json"
    summary_out = tmp / "hicache_prefetch_duplicate_transfer_ready_without_resident_summary.json"
    run_summary = tmp / "hicache_prefetch_duplicate_transfer_ready_without_resident_run.json"
    pages = ["p1", "p2", "p3"]
    trace_path.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(
                        10,
                        "hicache_prefetch_schedule_end",
                        {"event_role": "prefetch_schedule", "request_id": "req-a", "page_size": 64, "new_input_tokens": 192, "page_identity": pages},
                    ),
                    hicache_event(
                        20,
                        "hicache_l3_l2_transfer_end",
                        {
                            "event_role": "l3_to_l2_transfer",
                            "direction": "prefetch",
                            "tier_src": "L3",
                            "tier_dst": "L2",
                            "request_id": "req-a",
                            "page_size": 64,
                            "completed_tokens": 128,
                            "page_identity": pages,
                        },
                    ),
                    hicache_event(
                        30,
                        "hicache_l3_l2_transfer_end",
                        {
                            "event_role": "l3_to_l2_transfer",
                            "direction": "prefetch",
                            "tier_src": "L3",
                            "tier_dst": "L2",
                            "request_id": "req-a",
                            "page_size": 64,
                            "completed_tokens": 192,
                            "page_identity": pages,
                        },
                        pid=2,
                        tid=2,
                    ),
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    model_config.write_text(
        json.dumps({"modules": ["hicache"], "hicache": {"enabled": True, "page_size": 64, "prefetch_policy": "timeout"}}, ensure_ascii=False),
        encoding="utf-8",
    )
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(trace_path), "--run-summary", str(run_summary), "--model-config", str(model_config), "--model-summary", str(summary_out)],
        cwd=ROOT,
    )
    summary = json.loads(summary_out.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    assert summary["final_state"]["l2_resident_pages"] == ["p1", "p2"], summary
    assert summary["final_state"]["backuped_pages"] == ["p1", "p2"], summary
    assert summary["final_state"]["prefetch_ready_pages"] == ["p1", "p2", "p3"], summary
    assert summary["transitions_by_kind"]["add_l2_resident"] == 2, summary
    assert summary["transitions_by_kind"]["mark_prefetch_ready"] == 3, summary


def run_capacity_target_skips_source_remove_fixture(tmp: Path) -> None:
    """验证容量 what-if 不消费 base run 中观测到的 remove_page。"""

    trace_path = tmp / "hicache_capacity_skip_source_remove.json"
    model_config = tmp / "hicache_capacity_skip_source_remove_model.json"
    summary_out = tmp / "hicache_capacity_skip_source_remove_summary.json"
    run_summary = tmp / "hicache_capacity_skip_source_remove_run.json"
    trace_path.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(10, "hicache_insert_end", {"event_role": "insert", "page_size": 128, "page_identity": "p1"}),
                    hicache_event(20, "hicache_insert_end", {"event_role": "insert", "page_size": 128, "page_identity": "p2"}),
                    hicache_event(30, "hicache_remove_page_end", {"event_role": "remove_page", "page_size": 128, "tier_src": "GPU", "page_identity": "p1"}),
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    model_config.write_text(
        json.dumps({"modules": ["hicache"], "hicache": {"enabled": True, "page_size": 128, "l1_capacity_pages": 4}}, ensure_ascii=False),
        encoding="utf-8",
    )
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(trace_path), "--run-summary", str(run_summary), "--model-config", str(model_config), "--model-summary", str(summary_out)],
        cwd=ROOT,
    )
    summary = json.loads(summary_out.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    assert summary["skipped_non_invariant_events"] == 1, summary
    assert summary["final_state"]["l1_resident_pages"] == ["p1", "p2"], summary
    assert summary["final_state"]["evicted_pages"] == [], summary


def run_same_page_capacity_leaf_group_fixture(tmp: Path) -> None:
    """验证同 page size 的容量 what-if 按 radix leaf group 粒度释放。"""

    trace_path = tmp / "hicache_same_page_capacity_leaf_group.json"
    model_config = tmp / "hicache_same_page_capacity_leaf_group_model.json"
    summary_out = tmp / "hicache_same_page_capacity_leaf_group_summary.json"
    run_summary = tmp / "hicache_same_page_capacity_leaf_group_run.json"
    trace_path.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(10, "hicache_insert_end", {"event_role": "insert", "page_size": 128, "page_identity": ["p1", "p2", "p3"]}),
                    hicache_event(20, "hicache_insert_end", {"event_role": "insert", "page_size": 128, "page_identity": ["p4", "p5"]}),
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    model_config.write_text(
        json.dumps(
            {
                "modules": ["hicache"],
                "hicache": {
                    "enabled": True,
                    "page_size": 128,
                    "l1_capacity_pages": 3,
                    "l2_capacity_pages": 3,
                    "write_policy": "write_through",
                },
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(trace_path), "--run-summary", str(run_summary), "--model-config", str(model_config), "--model-summary", str(summary_out)],
        cwd=ROOT,
    )
    summary = json.loads(summary_out.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    assert summary["final_state"]["l1_resident_pages"] == ["p4", "p5"], summary
    assert summary["final_state"]["l2_resident_pages"] == ["p4", "p5"], summary
    assert summary["final_state"]["l3_resident_pages"] == ["p1", "p2", "p3", "p4", "p5"], summary
    assert summary["final_state"]["evicted_pages"] == [], summary
    assert summary["transitions_by_kind"]["remove_l1_resident"] == 3, summary
    assert summary["transitions_by_kind"]["remove_l2_resident"] == 3, summary


def run_prefetch_wait_complete_fixture(tmp: Path) -> None:
    """验证 wait_complete target 下，scheduled prefetch page 会进入 ready L2。"""

    trace_path = tmp / "hicache_prefetch_wait_complete.json"
    model_config = tmp / "hicache_prefetch_wait_complete_model.json"
    summary_out = tmp / "hicache_prefetch_wait_complete_summary.json"
    run_summary = tmp / "hicache_prefetch_wait_complete_run.json"
    trace_path.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(
                        8,
                        "hicache_prefetch_schedule_end",
                        {"event_role": "prefetch_schedule", "request_id": "req-wait", "page_size": 128, "page_identity": '["p1","p2"]'},
                    ),
                    hicache_event(10, "hicache_l3_prefetch_enqueue_end", {"event_role": "l3_prefetch_enqueue", "page_size": 128, "page_identity": '["p1","p2"]'}),
                    hicache_event(
                        15,
                        "hicache_prefetch_progress_start",
                        {
                            "event_role": "prefetch_progress",
                            "request_id": "req-wait",
                            "page_size": 128,
                            "prefetch_progress_state": {
                                "request_id": "req-wait",
                                "page_size": 128,
                                "operation_hash_pages": ["p1", "p2"],
                                "completed_tokens": 256,
                                "check_return": None,
                                "has_ongoing_prefetch": True,
                            },
                        },
                    ),
                    hicache_event(20, "hicache_l3_l2_transfer_end", {"event_role": "l3_to_l2_transfer", "page_size": 128, "tier_src": "L3", "tier_dst": "L2", "page_identity": "p1"}),
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    model_config.write_text(
        json.dumps({"modules": ["hicache"], "hicache": {"enabled": True, "page_size": 128, "prefetch_policy": "wait_complete"}}, ensure_ascii=False),
        encoding="utf-8",
    )
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(trace_path), "--run-summary", str(run_summary), "--model-config", str(model_config), "--model-summary", str(summary_out)],
        cwd=ROOT,
    )
    summary = json.loads(summary_out.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    assert summary["skipped_non_invariant_events"] == 1, summary
    assert summary["final_state"]["l2_resident_pages"] == ["p1", "p2"], summary
    assert summary["final_state"]["prefetch_planned_pages"] == ["p1", "p2"], summary
    assert summary["final_state"]["prefetch_ready_pages"] == ["p1", "p2"], summary
    assert summary["final_state"]["prefetch_late_pages"] == [], summary
    assert summary["final_state"]["prefetch_suppressed_pages"] == [], summary


def run_prefetch_wait_complete_suppressed_fixture(tmp: Path) -> None:
    """验证 wait_complete target 下缺少 ready evidence 的 planned page 会进入 suppressed。"""

    trace_path = tmp / "hicache_prefetch_wait_complete_suppressed.json"
    model_config = tmp / "hicache_prefetch_wait_complete_suppressed_model.json"
    summary_out = tmp / "hicache_prefetch_wait_complete_suppressed_summary.json"
    run_summary = tmp / "hicache_prefetch_wait_complete_suppressed_run.json"
    trace_path.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(
                        8,
                        "hicache_prefetch_schedule_end",
                        {
                            "event_role": "prefetch_schedule",
                            "request_id": "req-wait-suppressed",
                            "page_size": 128,
                            "page_identity": '["p1","p2"]',
                        },
                    ),
                    hicache_event(
                        10,
                        "hicache_l3_prefetch_enqueue_end",
                        {
                            "event_role": "l3_prefetch_enqueue",
                            "request_id": "req-wait-suppressed",
                            "page_size": 128,
                            "page_identity": '["p1","p2"]',
                        },
                    )
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    model_config.write_text(
        json.dumps({"modules": ["hicache"], "hicache": {"enabled": True, "page_size": 128, "prefetch_policy": "wait_complete"}}, ensure_ascii=False),
        encoding="utf-8",
    )
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(trace_path), "--run-summary", str(run_summary), "--model-config", str(model_config), "--model-summary", str(summary_out)],
        cwd=ROOT,
    )
    summary = json.loads(summary_out.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    assert summary["final_state"]["prefetch_planned_pages"] == ["p1", "p2"], summary
    assert summary["final_state"]["prefetch_ready_pages"] == [], summary
    assert summary["final_state"]["prefetch_suppressed_pages"] == ["p1", "p2"], summary
    assert summary["transitions_by_kind"]["mark_prefetch_suppressed"] == 2, summary


def run_prefetch_timeout_progress_fixture(tmp: Path) -> None:
    """验证 timeout target 可用 progress evidence 区分 ready 和 late page。"""

    trace_path = tmp / "hicache_prefetch_timeout_progress.json"
    model_config = tmp / "hicache_prefetch_timeout_progress_model.json"
    summary_out = tmp / "hicache_prefetch_timeout_progress_summary.json"
    run_summary = tmp / "hicache_prefetch_timeout_progress_run.json"
    trace_path.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(
                        10,
                        "hicache_prefetch_schedule_end",
                        {"event_role": "prefetch_schedule", "request_id": "req-timeout", "page_size": 64, "page_identity": ["p1", "p2"]},
                    ),
                    hicache_event(
                        20,
                        "hicache_prefetch_progress_start",
                        {
                            "event_role": "prefetch_progress",
                            "request_id": "req-timeout",
                            "page_size": 64,
                            "prefetch_done": False,
                            "prefetch_progress_state": {
                                "request_id": "req-timeout",
                                "page_size": 64,
                                "operation_hash_pages": ["p1", "p2"],
                                "completed_tokens": 64,
                                "check_return": None,
                                "has_ongoing_prefetch": True,
                            },
                        },
                    ),
                    hicache_event(
                        30,
                        "hicache_prefetch_progress_end",
                        {
                            "event_role": "prefetch_progress",
                            "request_id": "req-timeout",
                            "page_size": 64,
                            "prefetch_progress_state": {
                                "request_id": "req-timeout",
                                "page_size": 64,
                                "check_return": True,
                                "has_ongoing_prefetch": False,
                            },
                        },
                    ),
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    model_config.write_text(
        json.dumps(
            {
                "modules": ["hicache"],
                "hicache": {
                    "enabled": True,
                    "page_size": 64,
                    "prefetch_policy": "timeout",
                    "prefetch_timeout_base_sec": 0.0,
                    "prefetch_timeout_per_ki_token_sec": 0.0,
                    "prefetch_timeout_max_sec": 0.0,
                },
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(trace_path), "--run-summary", str(run_summary), "--model-config", str(model_config), "--model-summary", str(summary_out)],
        cwd=ROOT,
    )
    summary = json.loads(summary_out.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    assert summary["final_state"]["l2_resident_pages"] == ["p1"], summary
    assert summary["final_state"]["prefetch_planned_pages"] == ["p1", "p2"], summary
    assert summary["final_state"]["prefetch_ready_pages"] == ["p1"], summary
    assert summary["final_state"]["prefetch_late_pages"] == ["p2"], summary
    assert summary["final_state"]["prefetch_suppressed_pages"] == ["p2"], summary
    assert summary["transitions_by_kind"]["mark_prefetch_ready"] == 1, summary
    assert summary["transitions_by_kind"]["mark_prefetch_late"] == 1, summary
    assert summary["transitions_by_kind"]["mark_prefetch_suppressed"] == 1, summary


def run_prefetch_timeout_terminal_empty_fixture(tmp: Path) -> None:
    """验证 timeout 下 terminal empty progress 会 suppress pending pages。"""

    trace_path = tmp / "hicache_prefetch_timeout_terminal_empty.json"
    model_config = tmp / "hicache_prefetch_timeout_terminal_empty_model.json"
    summary_out = tmp / "hicache_prefetch_timeout_terminal_empty_summary.json"
    run_summary = tmp / "hicache_prefetch_timeout_terminal_empty_run.json"
    trace_path.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(
                        10,
                        "hicache_prefetch_schedule_end",
                        {"event_role": "prefetch_schedule", "request_id": "req-timeout-empty", "page_size": 128, "page_identity": ["p1", "p2"]},
                    ),
                    hicache_event(
                        20,
                        "hicache_prefetch_progress_end",
                        {
                            "event_role": "prefetch_progress",
                            "request_id": "req-timeout-empty",
                            "page_size": 128,
                            "prefetch_progress_state": {
                                "request_id": "req-timeout-empty",
                                "page_size": 128,
                                "check_return": True,
                                "has_ongoing_prefetch": False,
                            },
                        },
                    ),
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    model_config.write_text(
        json.dumps(
            {
                "modules": ["hicache"],
                "hicache": {
                    "enabled": True,
                    "page_size": 128,
                    "prefetch_policy": "timeout",
                    "prefetch_timeout_base_sec": 10.0,
                    "prefetch_timeout_per_ki_token_sec": 0.0,
                    "prefetch_timeout_max_sec": 10.0,
                },
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(trace_path), "--run-summary", str(run_summary), "--model-config", str(model_config), "--model-summary", str(summary_out)],
        cwd=ROOT,
    )
    summary = json.loads(summary_out.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    assert summary["final_state"]["prefetch_planned_pages"] == ["p1", "p2"], summary
    assert summary["final_state"]["prefetch_ready_pages"] == [], summary
    assert summary["final_state"]["prefetch_suppressed_pages"] == ["p1", "p2"], summary
    assert summary["transitions_by_kind"]["mark_prefetch_suppressed"] == 2, summary


def run_prefetch_timeout_finalize_fixture(tmp: Path) -> None:
    """验证 timeout target 缺少终止证据时不会在尾部强行 suppress。"""

    trace_path = tmp / "hicache_prefetch_timeout_finalize.json"
    model_config = tmp / "hicache_prefetch_timeout_finalize_model.json"
    summary_out = tmp / "hicache_prefetch_timeout_finalize_summary.json"
    run_summary = tmp / "hicache_prefetch_timeout_finalize_run.json"
    trace_path.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(
                        10,
                        "hicache_prefetch_schedule_end",
                        {"event_role": "prefetch_schedule", "request_id": "req-timeout-finalize", "page_size": 128, "page_identity": ["p1", "p2"]},
                    )
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    model_config.write_text(
        json.dumps(
            {
                "modules": ["hicache"],
                "hicache": {
                    "enabled": True,
                    "page_size": 128,
                    "prefetch_policy": "timeout",
                    "prefetch_timeout_base_sec": 10.0,
                    "prefetch_timeout_per_ki_token_sec": 0.0,
                    "prefetch_timeout_max_sec": 10.0,
                },
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(trace_path), "--run-summary", str(run_summary), "--model-config", str(model_config), "--model-summary", str(summary_out)],
        cwd=ROOT,
    )
    summary = json.loads(summary_out.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    assert summary["final_state"]["prefetch_planned_pages"] == ["p1", "p2"], summary
    assert summary["final_state"]["prefetch_ready_pages"] == [], summary
    assert summary["final_state"]["prefetch_late_pages"] == [], summary
    assert summary["final_state"]["prefetch_suppressed_pages"] == [], summary
    assert "mark_prefetch_suppressed" not in summary["transitions_by_kind"], summary


def run_prefetch_timeout_transfer_credit_fixture(tmp: Path) -> None:
    """验证 timeout 下已经完成的 transfer 仍作为 ready evidence。"""

    trace_path = tmp / "hicache_prefetch_timeout_transfer_credit.json"
    normal_config = tmp / "hicache_prefetch_timeout_transfer_credit_model.json"
    zero_config = tmp / "hicache_prefetch_timeout_transfer_credit_zero_model.json"
    normal_summary = tmp / "hicache_prefetch_timeout_transfer_credit_summary.json"
    zero_summary = tmp / "hicache_prefetch_timeout_transfer_credit_zero_summary.json"
    run_summary = tmp / "hicache_prefetch_timeout_transfer_credit_run.json"
    trace_path.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(
                        10,
                        "hicache_prefetch_schedule_end",
                        {"event_role": "prefetch_schedule", "request_id": "req-timeout-credit", "page_size": 128, "page_identity": ["p1", "p2"]},
                    ),
                    hicache_event(
                        20,
                        "hicache_l3_l2_transfer_end",
                        {
                            "event_role": "l3_to_l2_transfer",
                            "request_id": "req-timeout-credit",
                            "tier_src": "L3",
                            "tier_dst": "L2",
                            "page_size": 128,
                            "page_identity": ["p1", "p2"],
                        },
                    ),
                    hicache_event(
                        30,
                        "hicache_prefetch_progress_end",
                        {
                            "event_role": "prefetch_progress",
                            "request_id": "req-wb-credit",
                            "page_size": 128,
                            "prefetch_progress_state": {
                                "request_id": "req-wb-credit",
                                "page_size": 128,
                                "check_return": True,
                                "has_ongoing_prefetch": False,
                            },
                        },
                    ),
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    normal_config.write_text(
        json.dumps(
            {
                "modules": ["hicache"],
                "hicache": {
                    "enabled": True,
                    "page_size": 128,
                    "prefetch_policy": "timeout",
                    "prefetch_timeout_base_sec": 10.0,
                    "prefetch_timeout_per_ki_token_sec": 0.0,
                    "prefetch_timeout_max_sec": 10.0,
                },
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    zero_config.write_text(
        json.dumps(
            {
                "modules": ["hicache"],
                "hicache": {
                    "enabled": True,
                    "page_size": 128,
                    "prefetch_policy": "timeout",
                    "prefetch_timeout_base_sec": 0.0,
                    "prefetch_timeout_per_ki_token_sec": 0.0,
                    "prefetch_timeout_max_sec": 0.0,
                },
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(trace_path), "--run-summary", str(run_summary), "--model-config", str(normal_config), "--model-summary", str(normal_summary)],
        cwd=ROOT,
    )
    summary = json.loads(normal_summary.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    assert summary["final_state"]["prefetch_ready_pages"] == ["p1", "p2"], summary
    assert summary["final_state"]["prefetch_suppressed_pages"] == [], summary
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(trace_path), "--run-summary", str(run_summary), "--model-config", str(zero_config), "--model-summary", str(zero_summary)],
        cwd=ROOT,
    )
    zero = json.loads(zero_summary.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    assert zero["final_state"]["prefetch_ready_pages"] == ["p1", "p2"], zero
    assert zero["final_state"]["prefetch_suppressed_pages"] == [], zero
    assert zero["skipped_non_invariant_events"] == 0, zero


def run_prefetch_timeout_lock_ref_non_invariant_fixture(tmp: Path) -> None:
    """验证显式 timeout target 不消费 base prefetch policy 下观测到的 lock/ref。"""

    trace_path = tmp / "hicache_prefetch_timeout_lock_ref_non_invariant.json"
    model_config = tmp / "hicache_prefetch_timeout_lock_ref_non_invariant_model.json"
    summary_out = tmp / "hicache_prefetch_timeout_lock_ref_non_invariant_summary.json"
    run_summary = tmp / "hicache_prefetch_timeout_lock_ref_non_invariant_run.json"
    trace_path.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(
                        10,
                        "hicache_inc_lock_ref_end",
                        {
                            "event_role": "lock_ref_inc",
                            "page_size": 128,
                            "page_identity": ["p1"],
                        },
                    ),
                    hicache_event(
                        20,
                        "hicache_dec_lock_ref_end",
                        {
                            "event_role": "lock_ref_dec",
                            "page_size": 128,
                            "page_identity": ["p1"],
                        },
                    ),
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    model_config.write_text(
        json.dumps(
            {
                "modules": ["hicache"],
                "hicache": {
                    "enabled": True,
                    "page_size": 128,
                    "prefetch_policy": "timeout",
                    "prefetch_timeout_base_sec": 0.0,
                    "prefetch_timeout_per_ki_token_sec": 0.0,
                    "prefetch_timeout_max_sec": 0.0,
                },
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(trace_path), "--run-summary", str(run_summary), "--model-config", str(model_config), "--model-summary", str(summary_out)],
        cwd=ROOT,
    )
    summary = json.loads(summary_out.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    assert summary["skipped_non_invariant_events"] == 2, summary
    assert summary["final_state"].get("locked_pages", []) == [], summary
    assert "mark_locked" not in summary.get("transitions_by_kind", {}), summary
    assert "clear_locked" not in summary.get("transitions_by_kind", {}), summary


def run_prefetch_wait_complete_transfer_credit_fixture(tmp: Path) -> None:
    """验证 wait_complete target 也会消费 planned page 的 transfer completion。"""

    trace_path = tmp / "hicache_prefetch_wait_complete_transfer_credit.json"
    model_config = tmp / "hicache_prefetch_wait_complete_transfer_credit_model.json"
    summary_out = tmp / "hicache_prefetch_wait_complete_transfer_credit_summary.json"
    run_summary = tmp / "hicache_prefetch_wait_complete_transfer_credit_run.json"
    trace_path.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(
                        10,
                        "hicache_prefetch_schedule_end",
                        {"event_role": "prefetch_schedule", "request_id": "req-wait-credit", "page_size": 128, "page_identity": ["p1", "p2"]},
                    ),
                    hicache_event(
                        20,
                        "hicache_l3_l2_transfer_end",
                        {
                            "event_role": "l3_to_l2_transfer",
                            "request_id": "req-wait-credit",
                            "tier_src": "L3",
                            "tier_dst": "L2",
                            "page_size": 128,
                            "page_identity": ["p1", "p2"],
                        },
                    ),
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    model_config.write_text(
        json.dumps({"modules": ["hicache"], "hicache": {"enabled": True, "page_size": 128, "prefetch_policy": "wait_complete"}}, ensure_ascii=False),
        encoding="utf-8",
    )
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(trace_path), "--run-summary", str(run_summary), "--model-config", str(model_config), "--model-summary", str(summary_out)],
        cwd=ROOT,
    )
    summary = json.loads(summary_out.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    assert summary["skipped_non_invariant_events"] == 0, summary
    assert summary["final_state"]["l2_resident_pages"] == ["p1", "p2"], summary
    assert summary["final_state"]["prefetch_planned_pages"] == ["p1", "p2"], summary
    assert summary["final_state"]["prefetch_ready_pages"] == ["p1", "p2"], summary
    assert summary["final_state"]["prefetch_suppressed_pages"] == [], summary
    assert summary["transitions_by_kind"]["mark_prefetch_ready"] == 2, summary


def run_prefetch_wait_complete_lookup_l3_credit_fixture(tmp: Path) -> None:
    """验证 wait_complete target lookup 从 L3 推导出的 load 也会补 ready。"""

    trace_path = tmp / "hicache_prefetch_wait_complete_lookup_l3_credit.json"
    model_config = tmp / "hicache_prefetch_wait_complete_lookup_l3_credit_model.json"
    summary_out = tmp / "hicache_prefetch_wait_complete_lookup_l3_credit_summary.json"
    run_summary = tmp / "hicache_prefetch_wait_complete_lookup_l3_credit_run.json"
    trace_path.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(
                        10,
                        "hicache_lookup_end",
                        {
                            "event_role": "lookup",
                            "request_id": "req-plan",
                            "page_size": 64,
                            "page_identity": ["base_prefix", "base_ready"],
                            "target_page_identity": ["target_prefix", "target_ready"],
                        },
                    ),
                    hicache_event(
                        20,
                        "hicache_prefetch_schedule_end",
                        {
                            "event_role": "prefetch_schedule",
                            "request_id": "req-plan",
                            "page_size": 64,
                            "new_input_tokens": 128,
                            "page_identity": ["base_suffix_a", "base_suffix_b"],
                            "target_page_identity": ["stale_direct_target"],
                        },
                    ),
                    hicache_event(
                        30,
                        "hicache_insert_end",
                        {
                            "event_role": "insert",
                            "tier_dst": "L1",
                            "page_size": 128,
                            "page_identity": ["target_ready"],
                        },
                    ),
                    hicache_event(
                        40,
                        "hicache_insert_end",
                        {
                            "event_role": "insert",
                            "tier_dst": "L1",
                            "page_size": 128,
                            "page_identity": ["target_evictor"],
                        },
                    ),
                    hicache_event(
                        50,
                        "hicache_lookup_end",
                        {
                            "event_role": "lookup",
                            "request_id": "req-load",
                            "page_size": 64,
                            "page_identity": ["base_prefix", "base_ready"],
                            "target_page_identity": ["target_prefix", "target_ready"],
                        },
                    ),
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    model_config.write_text(
        json.dumps(
            {
                "modules": ["hicache"],
                "hicache": {
                    "enabled": True,
                    "page_size": 128,
                    "l1_capacity_pages": 1,
                    "l2_capacity_pages": 1,
                    "prefetch_policy": "wait_complete",
                    "write_policy": "write_through",
                },
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(trace_path), "--run-summary", str(run_summary), "--model-config", str(model_config), "--model-summary", str(summary_out)],
        cwd=ROOT,
    )
    summary = json.loads(summary_out.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    assert summary["skipped_non_invariant_events"] == 0, summary
    assert summary["final_state"]["l1_resident_pages"] == ["target_ready"], summary
    assert summary["final_state"]["l2_resident_pages"] == ["target_ready"], summary
    assert summary["final_state"]["prefetch_planned_pages"] == ["target_ready"], summary
    assert summary["final_state"]["prefetch_ready_pages"] == ["target_ready"], summary
    assert summary["final_state"]["prefetch_suppressed_pages"] == [], summary
    assert summary["transitions_by_kind"]["mark_prefetch_ready"] == 1, summary


def run_prefetch_best_effort_progress_fixture(tmp: Path) -> None:
    """验证 best_effort target 会在第一次 progress evidence 上终止 prefetch。"""

    trace_path = tmp / "hicache_prefetch_best_effort_progress.json"
    model_config = tmp / "hicache_prefetch_best_effort_progress_model.json"
    summary_out = tmp / "hicache_prefetch_best_effort_progress_summary.json"
    run_summary = tmp / "hicache_prefetch_best_effort_progress_run.json"
    trace_path.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(
                        10,
                        "hicache_prefetch_schedule_end",
                        {"event_role": "prefetch_schedule", "request_id": "req-best", "page_size": 64, "page_identity": ["p1", "p2"]},
                    ),
                    hicache_event(
                        20,
                        "hicache_prefetch_progress_start",
                        {
                            "event_role": "prefetch_progress",
                            "request_id": "req-best",
                            "page_size": 64,
                            "prefetch_done": False,
                            "prefetch_progress_state": {
                                "request_id": "req-best",
                                "page_size": 64,
                                "operation_hash_pages": ["p1", "p2"],
                                "completed_tokens": 0,
                                "check_return": None,
                                "has_ongoing_prefetch": True,
                            },
                        },
                    ),
                    hicache_event(
                        30,
                        "hicache_prefetch_progress_end",
                        {
                            "event_role": "prefetch_progress",
                            "request_id": "req-best",
                            "page_size": 64,
                            "prefetch_done": True,
                            "prefetch_progress_state": {
                                "request_id": "req-best",
                                "page_size": 64,
                                "check_return": True,
                                "has_ongoing_prefetch": False,
                            },
                        },
                    ),
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    model_config.write_text(
        json.dumps({"modules": ["hicache"], "hicache": {"enabled": True, "page_size": 64, "prefetch_policy": "best_effort"}}, ensure_ascii=False),
        encoding="utf-8",
    )
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(trace_path), "--run-summary", str(run_summary), "--model-config", str(model_config), "--model-summary", str(summary_out)],
        cwd=ROOT,
    )
    summary = json.loads(summary_out.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    assert summary["final_state"]["prefetch_planned_pages"] == ["p1", "p2"], summary
    assert summary["final_state"]["prefetch_ready_pages"] == [], summary
    assert summary["final_state"]["prefetch_late_pages"] == ["p1", "p2"], summary
    assert summary["final_state"]["prefetch_suppressed_pages"] == ["p1", "p2"], summary
    assert summary["transitions_by_kind"]["mark_prefetch_late"] == 2, summary
    assert summary["transitions_by_kind"]["mark_prefetch_suppressed"] == 2, summary


def run_prefetch_best_effort_transfer_credit_fixture(tmp: Path) -> None:
    """验证 best_effort 会把 planned page 的 L3->L2 transfer 当作 ready credit。"""

    trace_path = tmp / "hicache_prefetch_best_effort_transfer_credit.json"
    model_config = tmp / "hicache_prefetch_best_effort_transfer_credit_model.json"
    summary_out = tmp / "hicache_prefetch_best_effort_transfer_credit_summary.json"
    run_summary = tmp / "hicache_prefetch_best_effort_transfer_credit_run.json"
    trace_path.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(
                        10,
                        "hicache_prefetch_schedule_end",
                        {"event_role": "prefetch_schedule", "request_id": "req-best-credit", "page_size": 128, "page_identity": ["p1", "p2"]},
                    ),
                    hicache_event(
                        20,
                        "hicache_l3_l2_transfer_end",
                        {
                            "event_role": "l3_to_l2_transfer",
                            "request_id": "req-best-credit",
                            "tier_src": "L3",
                            "tier_dst": "L2",
                            "page_size": 128,
                            "page_identity": ["p1", "p2"],
                        },
                    ),
                    hicache_event(
                        30,
                        "hicache_prefetch_progress_end",
                        {
                            "event_role": "prefetch_progress",
                            "request_id": "req-wb-credit",
                            "page_size": 128,
                            "prefetch_progress_state": {
                                "request_id": "req-wb-credit",
                                "page_size": 128,
                                "check_return": True,
                                "has_ongoing_prefetch": False,
                            },
                        },
                    ),
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    model_config.write_text(
        json.dumps({"modules": ["hicache"], "hicache": {"enabled": True, "page_size": 128, "prefetch_policy": "best_effort"}}, ensure_ascii=False),
        encoding="utf-8",
    )
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(trace_path), "--run-summary", str(run_summary), "--model-config", str(model_config), "--model-summary", str(summary_out)],
        cwd=ROOT,
    )
    summary = json.loads(summary_out.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    assert summary["final_state"]["l2_resident_pages"] == ["p1", "p2"], summary
    assert summary["final_state"]["prefetch_planned_pages"] == ["p1", "p2"], summary
    assert summary["final_state"]["prefetch_ready_pages"] == ["p1", "p2"], summary
    assert summary["final_state"]["prefetch_late_pages"] == [], summary
    assert summary["final_state"]["prefetch_suppressed_pages"] == [], summary
    assert summary["transitions_by_kind"]["mark_prefetch_ready"] == 2, summary


def run_prefetch_best_effort_lock_ref_non_invariant_fixture(tmp: Path) -> None:
    """验证 best_effort target 不消费 base prefetch policy 下观测到的 lock/ref。"""

    trace_path = tmp / "hicache_prefetch_best_effort_lock_ref_non_invariant.json"
    model_config = tmp / "hicache_prefetch_best_effort_lock_ref_non_invariant_model.json"
    summary_out = tmp / "hicache_prefetch_best_effort_lock_ref_non_invariant_summary.json"
    run_summary = tmp / "hicache_prefetch_best_effort_lock_ref_non_invariant_run.json"
    trace_path.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(
                        10,
                        "hicache_inc_lock_ref_end",
                        {
                            "event_role": "lock_ref_inc",
                            "page_size": 128,
                            "page_identity": ["p1"],
                        },
                    ),
                    hicache_event(
                        20,
                        "hicache_dec_lock_ref_end",
                        {
                            "event_role": "lock_ref_dec",
                            "page_size": 128,
                            "page_identity": ["p1"],
                        },
                    ),
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    model_config.write_text(
        json.dumps(
            {
                "modules": ["hicache"],
                "hicache": {
                    "enabled": True,
                    "page_size": 128,
                    "prefetch_policy": "best_effort",
                },
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(trace_path), "--run-summary", str(run_summary), "--model-config", str(model_config), "--model-summary", str(summary_out)],
        cwd=ROOT,
    )
    summary = json.loads(summary_out.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    assert summary["skipped_non_invariant_events"] == 2, summary
    assert summary["final_state"].get("locked_pages", []) == [], summary
    assert "mark_locked" not in summary.get("transitions_by_kind", {}), summary
    assert "clear_locked" not in summary.get("transitions_by_kind", {}), summary


def run_prefetch_write_back_transfer_non_invariant_fixture(tmp: Path) -> None:
    """验证 write-back target 不消费 base write-through 的 L3->L2 prefetch completion。"""

    trace_path = tmp / "hicache_prefetch_write_back_transfer_non_invariant.json"
    model_config = tmp / "hicache_prefetch_write_back_transfer_non_invariant_model.json"
    summary_out = tmp / "hicache_prefetch_write_back_transfer_non_invariant_summary.json"
    run_summary = tmp / "hicache_prefetch_write_back_transfer_non_invariant_run.json"
    trace_path.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(
                        10,
                        "hicache_prefetch_schedule_end",
                        {"event_role": "prefetch_schedule", "request_id": "req-wb-credit", "page_size": 128, "page_identity": ["p1", "p2"]},
                    ),
                    hicache_event(
                        20,
                        "hicache_prefetch_progress_start",
                        {
                            "event_role": "prefetch_progress",
                            "request_id": "req-wb-credit",
                            "page_size": 128,
                            "prefetch_progress_state": {
                                "request_id": "req-wb-credit",
                                "page_size": 128,
                                "operation_hash_pages": ["p1", "p2"],
                                "completed_tokens": 256,
                                "check_return": None,
                                "has_ongoing_prefetch": True,
                            },
                        },
                    ),
                    hicache_event(
                        30,
                        "hicache_l3_l2_transfer_end",
                        {
                            "event_role": "l3_to_l2_transfer",
                            "request_id": "req-wb-credit",
                            "tier_src": "L3",
                            "tier_dst": "L2",
                            "page_size": 128,
                            "page_identity": ["p1", "p2"],
                        },
                    ),
                    hicache_event(
                        40,
                        "hicache_prefetch_progress_end",
                        {
                            "event_role": "prefetch_progress",
                            "request_id": "req-wb-credit",
                            "page_size": 128,
                            "prefetch_progress_state": {
                                "request_id": "req-wb-credit",
                                "page_size": 128,
                                "check_return": True,
                                "has_ongoing_prefetch": False,
                            },
                        },
                    ),
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    model_config.write_text(
        json.dumps(
            {
                "modules": ["hicache"],
                "hicache": {
                    "enabled": True,
                    "page_size": 128,
                    "write_policy": "write_back",
                    "prefetch_policy": "timeout",
                    "prefetch_timeout_base_sec": 10.0,
                    "prefetch_timeout_per_ki_token_sec": 0.0,
                    "prefetch_timeout_max_sec": 10.0,
                },
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(trace_path), "--run-summary", str(run_summary), "--model-config", str(model_config), "--model-summary", str(summary_out)],
        cwd=ROOT,
    )
    summary = json.loads(summary_out.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    assert summary["skipped_non_invariant_events"] == 1, summary
    assert summary["final_state"]["l2_resident_pages"] == [], summary
    assert summary["final_state"]["prefetch_planned_pages"] == ["p1", "p2"], summary
    assert summary["final_state"]["prefetch_ready_pages"] == [], summary
    assert summary["final_state"]["prefetch_suppressed_pages"] == ["p1", "p2"], summary


def run_prefetch_write_back_capacity_transfer_credit_fixture(tmp: Path) -> None:
    """验证 write-back + capacity target 可消费 L3->L2 transfer 作为 ready evidence。"""

    trace_path = tmp / "hicache_prefetch_write_back_capacity_transfer_credit.json"
    model_config = tmp / "hicache_prefetch_write_back_capacity_transfer_credit_model.json"
    summary_out = tmp / "hicache_prefetch_write_back_capacity_transfer_credit_summary.json"
    run_summary = tmp / "hicache_prefetch_write_back_capacity_transfer_credit_run.json"
    trace_path.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(
                        10,
                        "hicache_prefetch_schedule_end",
                        {"event_role": "prefetch_schedule", "request_id": "req-wb-cap-credit", "page_size": 128, "page_identity": ["p1"]},
                    ),
                    hicache_event(
                        20,
                        "hicache_l3_l2_transfer_end",
                        {
                            "event_role": "l3_to_l2_transfer",
                            "request_id": "req-wb-cap-credit",
                            "tier_src": "L3",
                            "tier_dst": "L2",
                            "page_size": 128,
                            "page_identity": ["p1"],
                        },
                    ),
                    hicache_event(
                        30,
                        "hicache_lookup_end",
                        {"event_role": "lookup", "request_id": "req-wb-cap-credit", "page_size": 128, "page_identity": ["p1"]},
                    ),
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    model_config.write_text(
        json.dumps(
            {
                "modules": ["hicache"],
                "hicache": {
                    "enabled": True,
                    "page_size": 128,
                    "l1_capacity_pages": 1,
                    "l2_capacity_pages": 1,
                    "write_policy": "write_back",
                    "prefetch_policy": "timeout",
                    "prefetch_timeout_base_sec": 10.0,
                    "prefetch_timeout_per_ki_token_sec": 0.0,
                    "prefetch_timeout_max_sec": 10.0,
                    "write_back_prefetch_transfer_credit": True,
                },
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(trace_path), "--run-summary", str(run_summary), "--model-config", str(model_config), "--model-summary", str(summary_out)],
        cwd=ROOT,
    )
    summary = json.loads(summary_out.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    assert summary["skipped_non_invariant_events"] == 0, summary
    assert summary["final_state"]["l1_resident_pages"] == ["p1"], summary
    assert summary["final_state"]["l2_resident_pages"] == ["p1"], summary
    assert summary["final_state"]["prefetch_ready_pages"] == ["p1"], summary
    assert summary["final_state"]["prefetch_suppressed_pages"] == [], summary
    assert summary["transitions_by_kind"]["mark_prefetch_ready"] == 1, summary


def run_same_page_policy_evict_leaf_group_fixture(tmp: Path) -> None:
    """验证 same page-size policy eviction 按 radix leaf group 粒度释放。"""

    trace_path = tmp / "hicache_same_page_policy_evict_leaf_group.json"
    model_config = tmp / "hicache_same_page_policy_evict_leaf_group_model.json"
    summary_out = tmp / "hicache_same_page_policy_evict_leaf_group_summary.json"
    run_summary = tmp / "hicache_same_page_policy_evict_leaf_group_run.json"
    trace_path.write_text(
        json.dumps(
            {
                "traceEvents": [
                    hicache_event(10, "hicache_insert_end", {"event_role": "insert", "page_size": 64, "tier_dst": "L1", "page_identity": ["p1", "p2", "p3"]}),
                    hicache_event(20, "hicache_insert_end", {"event_role": "insert", "page_size": 64, "tier_dst": "L1", "page_identity": ["p4"]}),
                    hicache_event(30, "hicache_evict_end", {"event_role": "evict_summary", "page_size": 64, "requested_tokens": 64}),
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    model_config.write_text(
        json.dumps(
            {
                "modules": ["hicache"],
                "hicache": {
                    "enabled": True,
                    "write_policy": "write_back",
                    "page_size": 64,
                    "l1_capacity_pages": 4,
                    "l2_capacity_pages": 8,
                },
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    subprocess.check_call(
        [str(ROOT / "build/bin/trace_graph"), "--input", str(trace_path), "--run-summary", str(run_summary), "--model-config", str(model_config), "--model-summary", str(summary_out)],
        cwd=ROOT,
    )
    summary = json.loads(summary_out.read_text(encoding="utf-8"))["modules"][0]["hicache"]
    final_state = summary["final_state"]
    assert final_state["l1_resident_pages"] == ["p4"], final_state
    assert final_state["l2_resident_pages"] == ["p1", "p2", "p3"], final_state
    assert final_state["dirty_pages"] == ["p4"], final_state
    assert final_state["backuped_pages"] == ["p1", "p2", "p3"], final_state
    assert final_state["evicted_pages"] == ["p1", "p2", "p3"], final_state
    assert summary["dirty_eviction_events"] == 3, summary
    assert summary["transitions_by_kind"]["mark_evicted"] == 3, summary


def hicache_event(ts: int, name: str, args: dict[str, object], pid: int = 1, tid: int = 1) -> dict[str, object]:
    base_args: dict[str, object] = {"domain": "python_probe", "event_kind": "hicache"}
    base_args.update(args)
    return {"name": name, "cat": "python_probe", "ph": "X", "ts": ts, "dur": 1, "pid": pid, "tid": tid, "args": base_args}


def state_snapshot_event(ts: int, snapshot: dict[str, object]) -> dict[str, object]:
    return {
        "name": "hicache_lookup_end:state_snapshot",
        "cat": "python_probe",
        "ph": "X",
        "ts": ts,
        "dur": 9999,
        "pid": 1,
        "tid": 1,
        "args": {
            "domain": "python_probe",
            "event_kind": "state_snapshot",
            "model_input": False,
            "state_snapshot": snapshot,
        },
    }


if __name__ == "__main__":
    raise SystemExit(main())
