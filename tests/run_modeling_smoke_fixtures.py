#!/usr/bin/env python3
"""Smoke fixtures for the Python modeling runner."""

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
        run_direct_fixture(tmp)
        run_manifest_merge_fixture(tmp)
        run_hicache_token_state_validation_fixture(tmp)
        run_hicache_required_oracle_gate_fixture(tmp)
        run_hicache_target_experiment_config_fixture(tmp)
    print("modeling smoke fixtures passed")
    return 0


def run_direct_fixture(tmp: Path) -> None:
    config = tmp / "modeling_direct.json"
    output_dir = tmp / "direct_out"
    write_json(
        config,
        {
            "input": {"trace_paths": [str(ROOT / "tests/fixtures/modeling/tracegraph_basic.json")]},
            "output_dir": str(output_dir),
            "mode": "faithful_replay",
            "outputs": {},
        },
    )
    run_model_runner(config)
    prediction = load_json(output_dir / "prediction.json")
    assert prediction["predicted_e2e_ns"] > 0, prediction
    assert not (output_dir / "dag_chrome_trace.json").exists()

    scale_config = tmp / "modeling_scale.json"
    scale_output_dir = tmp / "scale_out"
    write_json(
        scale_config,
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
    )
    run_model_runner(scale_config)
    cpp_model_config = load_json(scale_output_dir / "cpp_model_config.json")
    assert cpp_model_config["modules"] == ["node_scale"], cpp_model_config
    module_summary = load_json(scale_output_dir / "model_summary.json")
    assert module_summary["modules"][0]["name"] == "NodeScaleModule", module_summary
    assert module_summary["modules"][0]["scaled_nodes"] == 1, module_summary

    override_cpp_config = tmp / "override_cpp_model_config.json"
    write_json(
        override_cpp_config,
        {
            "modules": ["node_scale"],
            "node_scale": {"enabled": True, "rules": [{"id": "scale_compute_override", "name": "compute_use_kv", "factor": 3.0}]},
        },
    )
    override_config = tmp / "modeling_cpp_override.json"
    override_output_dir = tmp / "cpp_override_out"
    write_json(
        override_config,
        {
            "input": {"trace_paths": [str(ROOT / "tests/fixtures/modeling/tracegraph_basic.json")]},
            "output_dir": str(override_output_dir),
            "mode": "cache_patch",
            "outputs": {"emit_module_summary": True},
        },
    )
    run_model_runner(override_config, "--cpp-model-config", str(override_cpp_config))
    assert not (override_output_dir / "cpp_model_config.json").exists(), override_output_dir
    override_summary = load_json(override_output_dir / "model_summary.json")
    assert override_summary["modules"][0]["name"] == "NodeScaleModule", override_summary
    assert override_summary["modules"][0]["rules"][0]["id"] == "scale_compute_override", override_summary


def run_manifest_merge_fixture(tmp: Path) -> None:
    torch_trace = tmp / "trace/torch/rank0_pid123/ASCEND_PROFILER_OUTPUT/trace_view.json"
    ld_trace = tmp / "trace/ld_preload/cpu_trace.json.rank0.pid123.json"
    python_probe = tmp / "trace/python_probe/python_probe_trace.rank0.pid123.json"
    bench_jsonl = tmp / "bench/bench.jsonl"
    torch_trace.parent.mkdir(parents=True)
    ld_trace.parent.mkdir(parents=True)
    python_probe.parent.mkdir(parents=True)
    bench_jsonl.parent.mkdir(parents=True)

    write_json(
        torch_trace,
        {
            "traceEvents": [
                {"name": "process_name", "ph": "M", "pid": 123, "tid": 0, "args": {"name": "CANN"}},
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
    )
    write_json(
        ld_trace,
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
            {"name": "CPUInfer::sync", "cat": "cpu", "ph": "X", "ts": 20, "dur": 3, "pid": 123, "tid": 8, "args": {"domain": "cpuinfer"}},
        ],
    )
    write_json(
        python_probe,
        {
            "traceEvents": [
                {
                    "name": "hicache_prefetch_io_observed_end",
                    "cat": "python_probe",
                    "ph": "X",
                    "ts": 30,
                    "dur": 7,
                    "pid": 123,
                    "tid": 9,
                    "args": {
                        "domain": "python_probe",
                        "event_kind": "hicache",
                        "target_id": "hicache_controller.prefetch_io_observed",
                        "event_role": "prefetch_io_observed",
                        "fact_class": "timing_observation",
                        "state_model_input": False,
                        "dag_input": True,
                        "phase": "end",
                        "bytes": 64,
                    },
                }
            ]
        },
    )
    bench_jsonl.write_text(json.dumps({"duration": 0.0001, "completed": 1}) + "\n", encoding="utf-8")
    manifest = tmp / "profile_manifest.json"
    write_json(
        manifest,
        {
            "run_dir": str(tmp),
            "trace": {
                "torch_trace_files": [{"path": str(torch_trace), "exists": True}],
                "ld_preload_trace_files": [{"path": str(ld_trace), "exists": True}],
            },
            "sidecar": {"python_probe_files": [{"path": str(python_probe), "exists": True}]},
        },
    )
    config = tmp / "modeling_manifest.json"
    output_dir = tmp / "manifest_out"
    write_json(
        config,
        {
            "input": {"profile_manifest": str(manifest)},
            "output_dir": str(output_dir),
            "mode": "cache_patch",
            "cpp_model_config": {"modules": ["hicache"], "hicache": {"enabled": True, "page_size": 128}},
        },
    )
    run_model_runner(config, "--emit-module-summary", "--emit-dag-chrome-trace", "--emit-validation")
    merged = output_dir / "merged_trace/merged_trace_00.json"
    assert merged.is_file(), merged
    merged_events = load_json(merged)["traceEvents"]
    assert any(event["args"].get("Raw Stream") == "s7" for event in merged_events), merged_events
    assert any(event.get("name") == "CPUInfer::sync" for event in merged_events), merged_events
    assert any(event.get("name") == "hicache_prefetch_io_observed_end" for event in merged_events), merged_events
    validation = load_json(output_dir / "validation.json")
    assert validation["workload_window"]["source"] == "sglang_bench_serving_duration", validation
    assert validation["workload_window"]["actual_e2e_ns"] == 100000, validation


def run_hicache_token_state_validation_fixture(tmp: Path) -> None:
    page_size = 4
    scope = "scope0"
    tokens = [1, 2, 3, 4, 5, 6, 7, 8]
    expected_pages = sorted(page_ids(tokens, page_size, scope))
    trace_path = tmp / "hicache_token_trace.json"
    output_dir = tmp / "hicache_token_out"
    write_json(
        trace_path,
        {
            "traceEvents": [
                invariant_event(
                    10,
                    "insert_path",
                    "hiradix.insert",
                    {
                        "request_id": "req-token",
                        "cache_scope": scope,
                        "source_page_size": page_size,
                        "seq_no": 1,
                        "token_dictionary": token_dictionary("token-path", tokens),
                        "full_path_span": token_span("token-path", 0, len(tokens)),
                        "token_count": len(tokens),
                        "value_token_count": len(tokens),
                        "prefix_len": 0,
                    },
                )
            ]
        },
    )
    config = tmp / "hicache_token_modeling.json"
    write_json(
        config,
        {
            "input": {"trace_paths": [str(trace_path)]},
            "output_dir": str(output_dir),
            "mode": "cache_state",
            "validation": {"hicache_state": {"enabled": True}},
            "modules": [
                {
                    "name": "HiCacheModule",
                    "enabled": True,
                    "config": {"hicache": {"enabled": True, "page_size": page_size, "write_policy": "write_through"}},
                }
            ],
            "outputs": {"emit_module_summary": True, "emit_validation": True},
        },
    )
    run_model_runner(config, "--emit-validation", "--emit-module-summary")
    summary = load_json(output_dir / "model_summary.json")["modules"][0]["hicache"]
    final_state = summary["final_state"]
    assert summary["missing_invariant_facts"] == {}, summary
    assert summary["skipped_non_invariant_events"] == 0, summary
    assert final_state["l1_resident_pages"] == expected_pages, final_state
    assert final_state["l2_resident_pages"] == expected_pages, final_state
    assert final_state["l3_resident_pages"] == expected_pages, final_state

    validation = load_json(output_dir / "validation.json")
    hicache = validation["hicache_state"]
    assert validation["validation_ready"] is True, validation
    assert validation["validation_errors"] == [], validation
    assert hicache["state_trace_ready"] is False, validation
    assert hicache["oracle_state_validation_required"] is False, validation
    assert hicache["final_state_match"] is None, validation
    assert hicache["invariant_coverage_ready"] is True, validation
    assert hicache["missing_invariant_facts"] == [], validation
    assert hicache["predicted_state_trace_ready"] is True, validation


def run_hicache_required_oracle_gate_fixture(tmp: Path) -> None:
    trace_path = tmp / "hicache_no_oracle_trace.json"
    output_dir = tmp / "hicache_no_oracle_out"
    write_json(trace_path, {"traceEvents": []})
    config = tmp / "hicache_no_oracle_modeling.json"
    write_json(
        config,
        {
            "input": {"trace_paths": [str(trace_path)]},
            "output_dir": str(output_dir),
            "mode": "cache_state",
            "validation": {"hicache_state": {"enabled": True, "require_oracle_state_trace": True}},
            "modules": [{"name": "HiCacheModule", "enabled": True, "config": {"hicache": {"enabled": True, "page_size": 4}}}],
            "outputs": {"emit_module_summary": True, "emit_validation": True},
        },
    )
    run_model_runner(config, "--emit-validation", "--emit-module-summary")
    validation = load_json(output_dir / "validation.json")
    assert validation["validation_ready"] is False, validation
    assert "hicache_state_trace_not_ready" in validation["validation_errors"], validation
    assert validation["hicache_state"]["oracle_state_validation_required"] is True, validation


def run_hicache_target_experiment_config_fixture(tmp: Path) -> None:
    output_dir = tmp / "target_experiment_out"
    config = tmp / "target_experiment_modeling.json"
    write_json(
        config,
        {
            "input": {
                "trace_paths": [str(ROOT / "tests/fixtures/modeling/tracegraph_basic.json")],
                "target_experiment_config": {
                    "server": {
                        "command": [
                            "python3",
                            "-m",
                            "sglang.launch_server",
                            "--page-size",
                            "4",
                            "--hicache-write-policy",
                            "write_through_selective",
                            "--hicache-storage-prefetch-policy",
                            "wait_complete",
                        ]
                    },
                    "modeling": {"hicache": {"l1_capacity_pages": 2, "l2_capacity_pages": 3}},
                },
            },
            "output_dir": str(output_dir),
            "mode": "cache_state",
            "outputs": {"emit_module_summary": True},
        },
    )
    run_model_runner(config)
    cpp_cfg = load_json(output_dir / "cpp_model_config.json")
    assert cpp_cfg["modules"] == ["hicache"], cpp_cfg
    assert cpp_cfg["hicache"]["page_size"] == 4, cpp_cfg
    assert cpp_cfg["hicache"]["write_policy"] == "write_through_selective", cpp_cfg
    assert cpp_cfg["hicache"]["prefetch_policy"] == "wait_complete", cpp_cfg
    assert "storage_prefetch_policy" not in cpp_cfg["hicache"], cpp_cfg
    assert cpp_cfg["hicache"]["l1_capacity_pages"] == 2, cpp_cfg
    assert cpp_cfg["hicache"]["l2_capacity_pages"] == 3, cpp_cfg


def invariant_event(ts: int, role: str, target_id: str, args: dict[str, Any]) -> dict[str, Any]:
    base_args: dict[str, Any] = {
        "domain": "python_probe",
        "event_kind": "hicache",
        "target_id": target_id,
        "event_role": role,
        "fact_class": "invariant_state",
        "state_model_input": True,
        "dag_input": False,
        "model_input": True,
        "phase": "end",
    }
    base_args.update(args)
    return {"name": f"hicache_{role}_end", "cat": "python_probe", "ph": "X", "ts": ts, "dur": 1, "pid": 1, "tid": 1, "args": base_args}


def token_dictionary(path_id: str, tokens: list[int]) -> dict[str, Any]:
    return {"token_path_id": path_id, "token_ids": tokens, "token_count": len(tokens), "hash_algo": HASH_ALGO}


def token_span(path_id: str, begin: int, end: int) -> dict[str, Any]:
    return {"path_id": path_id, "begin": begin, "end": end, "token_count": end - begin, "hash_algo": HASH_ALGO}


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


def run_model_runner(config: Path, *extra_args: str) -> None:
    subprocess.check_call([sys.executable, str(ROOT / "scripts/internal/model_runner.py"), "--config", str(config), *extra_args], cwd=ROOT)


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=True, indent=2) + "\n", encoding="utf-8")


def load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


if __name__ == "__main__":
    raise SystemExit(main())
