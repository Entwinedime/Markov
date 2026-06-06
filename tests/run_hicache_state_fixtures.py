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
        assert cache_summary["dirty_eviction_events"] == 1, cache_summary
        assert cache_summary["missing_page_identity_events"] == 0, cache_summary
        final_state = cache_summary["final_state"]
        assert final_state["l1_resident_pages"] == ["p1", "p3"], final_state
        assert final_state["l2_resident_pages"] == ["p1", "p3", "p4"], final_state
        assert final_state["l3_resident_pages"] == ["p1", "p4"], final_state
        assert final_state["dirty_pages"] == ["p2"], final_state
        assert final_state["backuped_pages"] == ["p1"], final_state
        assert final_state["evicted_pages"] == ["p2"], final_state
        assert final_state["prefetch_planned_pages"] == ["p4"], final_state
        assert final_state["prefetch_ready_pages"] == ["p4"], final_state
        assert cache_summary["transitions_by_kind"]["mark_dirty"] == 2, cache_summary
        assert cache_summary["transitions_by_kind"]["clear_dirty"] == 1, cache_summary
        assert cache_summary["transitions_by_kind"]["mark_evicted"] == 1, cache_summary
        assert "pages_by_edge" not in cache_summary, cache_summary
        run = json.loads(run_summary.read_text(encoding="utf-8"))
        assert run["simulated_e2e_ns"] == run["real_e2e_ns"], run
    print("hicache state fixtures passed")
    return 0


def hicache_event(ts: int, name: str, args: dict[str, object]) -> dict[str, object]:
    base_args: dict[str, object] = {"domain": "python_probe", "event_kind": "hicache"}
    base_args.update(args)
    return {"name": name, "cat": "python_probe", "ph": "X", "ts": ts, "dur": 1, "pid": 1, "tid": 1, "args": base_args}


if __name__ == "__main__":
    raise SystemExit(main())
