#!/usr/bin/env python3
"""C++ HiCache skeleton module fixtures。"""

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
                        {
                            "name": "hicache_l2_l1_enqueue_end",
                            "cat": "python_probe",
                            "ph": "X",
                            "ts": 10,
                            "dur": 1,
                            "pid": 1,
                            "tid": 1,
                            "args": {
                                "domain": "python_probe",
                                "event_kind": "movement",
                                "direction": "load",
                                "tier_src": "L2",
                                "tier_dst": "L1",
                                "num_pages": 2,
                                "bytes": 128,
                                "page_identity": "p1|p2",
                            },
                        },
                        {
                            "name": "hicache_write_storage_schedule_end",
                            "cat": "python_probe",
                            "ph": "X",
                            "ts": 20,
                            "dur": 1,
                            "pid": 1,
                            "tid": 1,
                            "args": {
                                "domain": "python_probe",
                                "event_kind": "movement",
                                "direction": "write",
                                "tier_src": "L2",
                                "tier_dst": "L3",
                                "num_pages": 1,
                                "bytes": 64,
                                "page_identity": "p1",
                            },
                        },
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
        assert cache_summary["status"] == "skeleton", cache_summary
        assert cache_summary["input_hicache_events"] == 2, cache_summary
        assert cache_summary["dag_mutations"] == 0, cache_summary
        assert "pages_by_edge" not in cache_summary, cache_summary
        assert json.loads(run_summary.read_text(encoding="utf-8"))["simulated_e2e_ns"] == 2
    print("hicache state fixtures passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
