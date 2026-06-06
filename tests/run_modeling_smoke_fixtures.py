#!/usr/bin/env python3
"""C++ modeling smoke fixtures。"""

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
        run_direct_fixture(tmp)
        run_manifest_merge_fixture(tmp)
    print("modeling smoke fixtures passed")
    return 0


def run_direct_fixture(tmp: Path) -> None:
    """验证 runner 直接调用 C++ TraceGraph。"""

    config = tmp / "modeling_direct.json"
    output_dir = tmp / "direct_out"
    config.write_text(
        json.dumps(
            {
                "input": {"trace_paths": [str(ROOT / "tests/fixtures/modeling/tracegraph_basic.json")]},
                "output_dir": str(output_dir),
                "mode": "faithful_replay",
                "outputs": {},
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    subprocess.check_call(
        [
            sys.executable,
            str(ROOT / "scripts/internal/model_runner.py"),
            "--config",
            str(config),
        ],
        cwd=ROOT,
    )
    prediction = json.loads((output_dir / "prediction.json").read_text(encoding="utf-8"))
    assert prediction["predicted_e2e_ns"] > 0, prediction
    assert not (output_dir / "dag_chrome_trace.json").exists()

    scale_config = tmp / "modeling_scale.json"
    scale_output_dir = tmp / "scale_out"
    scale_config.write_text(
        json.dumps(
            {
                "input": {"trace_paths": [str(ROOT / "tests/fixtures/modeling/tracegraph_basic.json")]},
                "output_dir": str(scale_output_dir),
                "mode": "cache_patch",
                "modules": [
                    {
                        "name": "NodeScaleModule",
                        "enabled": True,
                        "config": {"rules": [{"id": "scale_compute", "name": "compute_use_kv", "factor": 2.0}]},
                    }
                ],
                "outputs": {"emit_module_summary": True},
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    subprocess.check_call([sys.executable, str(ROOT / "scripts/internal/model_runner.py"), "--config", str(scale_config)], cwd=ROOT)
    cpp_model_config = json.loads((scale_output_dir / "cpp_model_config.json").read_text(encoding="utf-8"))
    assert cpp_model_config["modules"] == ["node_scale"], cpp_model_config
    module_summary = json.loads((scale_output_dir / "model_summary.json").read_text(encoding="utf-8"))
    assert module_summary["modules"][0]["name"] == "NodeScaleModule", module_summary
    assert module_summary["modules"][0]["scaled_nodes"] == 1, module_summary

    faithful_config = tmp / "modeling_faithful_with_modules.json"
    faithful_output_dir = tmp / "faithful_out"
    faithful_config.write_text(
        json.dumps(
            {
                "input": {"trace_paths": [str(ROOT / "tests/fixtures/modeling/tracegraph_basic.json")]},
                "output_dir": str(faithful_output_dir),
                "mode": "faithful_replay",
                "modules": [
                    {
                        "name": "NodeScaleModule",
                        "enabled": True,
                        "config": {"rules": [{"id": "scale_compute", "name": "compute_use_kv", "factor": 2.0}]},
                    }
                ],
                "outputs": {"emit_module_summary": True, "emit_validation": True},
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    subprocess.check_call([sys.executable, str(ROOT / "scripts/internal/model_runner.py"), "--config", str(faithful_config)], cwd=ROOT)
    assert not (faithful_output_dir / "cpp_model_config.json").exists()
    faithful_summary = json.loads((faithful_output_dir / "model_summary.json").read_text(encoding="utf-8"))
    assert faithful_summary["modules"] == [], faithful_summary
    faithful_validation = json.loads((faithful_output_dir / "validation.json").read_text(encoding="utf-8"))
    assert faithful_validation["dag"]["dag_mutation_count"] == 0, faithful_validation


def run_manifest_merge_fixture(tmp: Path) -> None:
    """验证 manifest 入口会先合并三类 trace，再交给 C++ 后端。"""

    torch_trace = tmp / "trace/torch/rank0_pid123/ASCEND_PROFILER_OUTPUT/trace_view.json"
    ld_trace = tmp / "trace/ld_preload/cpu_trace.json.rank0.pid123.json"
    python_probe = tmp / "trace/python_probe/python_probe_trace.rank0.pid123.json"
    bench_jsonl = tmp / "bench/bench.jsonl"
    torch_trace.parent.mkdir(parents=True)
    ld_trace.parent.mkdir(parents=True)
    python_probe.parent.mkdir(parents=True)
    bench_jsonl.parent.mkdir(parents=True)
    torch_trace.write_text(
        json.dumps(
            {
                "traceEvents": [
                    {
                        "name": "process_name",
                        "ph": "M",
                        "pid": 123,
                        "tid": 0,
                        "args": {"name": "CANN"},
                    },
                    {
                        "name": "AscendCL@aclrtSynchronizeStream",
                        "cat": "Kernel",
                        "ph": "X",
                        "ts": 10,
                        "dur": 5,
                        "pid": 123,
                        "tid": 7,
                        "args": {"Physic Stream Id": "7"},
                    },
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    ld_trace.write_text(
        json.dumps(
            [
                {
                    "name": "AscendCL@aclrtSynchronizeStream",
                    "cat": "ld_preload",
                    "ph": "X",
                    "ts": 10,
                    "dur": 5,
                    "pid": 123,
                    "tid": 7,
                    "args": {"Function-Args": {"stream": "s7"}, "domain": "ld_preload"},
                },
                {
                    "name": "CPUInfer::sync",
                    "cat": "cpu",
                    "ph": "X",
                    "ts": 20,
                    "dur": 3,
                    "pid": 123,
                    "tid": 8,
                    "args": {"domain": "cpuinfer"},
                },
            ],
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    python_probe.write_text(
        json.dumps(
            {
                "traceEvents": [
                    {
                        "name": "hicache_l2_l1_enqueue_end",
                        "cat": "python_probe",
                        "ph": "X",
                        "ts": 30,
                        "dur": 7,
                        "pid": 123,
                        "tid": 9,
                        "args": {
                            "domain": "python_probe",
                            "event_kind": "movement",
                            "direction": "load",
                            "tier_src": "L2",
                            "tier_dst": "L1",
                            "num_pages": 1,
                            "bytes": 64,
                        },
                    }
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    bench_jsonl.write_text(json.dumps({"duration": 0.0001, "completed": 1}) + "\n", encoding="utf-8")
    manifest = tmp / "profile_manifest.json"
    manifest.write_text(
        json.dumps(
            {
                "run_dir": str(tmp),
                "trace": {
                    "torch_trace_files": [{"path": str(torch_trace), "exists": True}],
                    "ld_preload_trace_files": [{"path": str(ld_trace), "exists": True}],
                },
                "sidecar": {"python_probe_files": [{"path": str(python_probe), "exists": True}]},
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    config = tmp / "modeling_manifest.json"
    output_dir = tmp / "manifest_out"
    config.write_text(
        json.dumps(
            {
                "input": {"profile_manifest": str(manifest)},
                "output_dir": str(output_dir),
                "mode": "cache_patch",
                "validation": {"faithful_replay_full_e2e_rel_error_max": 1000.0},
                "cpp_model_config": {
                    "modules": ["hicache"],
                    "hicache": {"enabled": True},
                },
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    subprocess.check_call(
        [
            sys.executable,
            str(ROOT / "scripts/internal/model_runner.py"),
            "--config",
            str(config),
            "--emit-module-summary",
            "--emit-dag-chrome-trace",
            "--emit-validation",
        ],
        cwd=ROOT,
    )
    merged = output_dir / "merged_trace/merged_trace_00.json"
    assert merged.is_file(), merged
    merged_events = json.loads(merged.read_text(encoding="utf-8"))["traceEvents"]
    assert any(event["args"].get("Raw Stream") == "s7" for event in merged_events), merged_events
    assert any(event.get("name") == "CPUInfer::sync" for event in merged_events), merged_events
    assert any(event.get("name") == "hicache_l2_l1_enqueue_end" for event in merged_events), merged_events
    summary = json.loads((output_dir / "model_summary.json").read_text(encoding="utf-8"))
    assert summary["modules"], summary
    validation = json.loads((output_dir / "validation.json").read_text(encoding="utf-8"))
    assert validation["workload_window"]["source"] == "sglang_bench_serving_duration", validation
    assert validation["workload_window"]["actual_e2e_ns"] == 100000, validation


if __name__ == "__main__":
    raise SystemExit(main())
