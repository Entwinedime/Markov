#!/usr/bin/env python3
"""C++ modeling smoke fixtures。"""

from __future__ import annotations

import json
import importlib.util
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
        run_hicache_state_validation_fixture(tmp)
        run_hicache_state_ignore_keys_fixture(tmp)
        run_hicache_lock_fact_oracle_fixture(tmp)
        run_hicache_dirty_derivation_fixture()
        run_hicache_capacity_config_audit_fixture()
        run_hicache_target_experiment_config_fixture(tmp)
        run_hicache_observed_policy_rejected_fixture()
        run_hicache_l3_evidence_only_fixture(tmp)
        run_hicache_state_prediction_fixture(tmp)
        run_hicache_prefetch_oracle_fixture(tmp)
        run_hicache_prefetch_transfer_oracle_fixture(tmp)
        run_hicache_prefetch_timeout_oracle_fixture(tmp)
        run_hicache_prefetch_suppressed_oracle_fixture(tmp)
        run_hicache_state_mismatch_fixture(tmp)
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

    override_cpp_config = tmp / "override_cpp_model_config.json"
    override_cpp_config.write_text(
        json.dumps(
            {
                "modules": ["node_scale"],
                "node_scale": {"enabled": True, "rules": [{"id": "scale_compute_override", "name": "compute_use_kv", "factor": 3.0}]},
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    override_config = tmp / "modeling_cpp_override.json"
    override_output_dir = tmp / "cpp_override_out"
    override_config.write_text(
        json.dumps(
            {
                "input": {"trace_paths": [str(ROOT / "tests/fixtures/modeling/tracegraph_basic.json")]},
                "output_dir": str(override_output_dir),
                "mode": "cache_patch",
                "outputs": {"emit_module_summary": True},
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
            str(override_config),
            "--cpp-model-config",
            str(override_cpp_config),
        ],
        cwd=ROOT,
    )
    assert not (override_output_dir / "cpp_model_config.json").exists(), override_output_dir
    override_summary = json.loads((override_output_dir / "model_summary.json").read_text(encoding="utf-8"))
    assert override_summary["modules"][0]["name"] == "NodeScaleModule", override_summary
    assert override_summary["modules"][0]["rules"][0]["id"] == "scale_compute_override", override_summary

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
                    "hicache": {
                        "enabled": True,
                        "page_size": 128,
                        "l1_capacity_pages": 64,
                        "l2_capacity_pages": 128,
                        "write_policy": "write_back",
                        "prefetch_policy": "best_effort",
                    },
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


def run_hicache_state_validation_fixture(tmp: Path) -> None:
    """验证 cache_state validation 能对比 C++ state 和 oracle snapshot。"""

    sidecar = tmp / "state_validation/trace/python_probe/python_probe_trace.rank0.pid456.json"
    sidecar.parent.mkdir(parents=True)
    sidecar.write_text(
        json.dumps(
            {
                "traceEvents": [
                    {
                        "name": "hicache_insert_start",
                        "cat": "python_probe",
                        "ph": "X",
                        "ts": 10,
                        "dur": 0,
                        "pid": 456,
                        "tid": 1,
                        "args": {
                            "domain": "python_probe",
                            "event_kind": "hicache_insert_start",
                            "model_input": True,
                            "event_role": "insert",
                            "request_id": "req-p12",
                            "tier_dst": "L1",
                            "page_identity": ["p1", "p2"],
                        },
                    },
                    {
                        "name": "hicache_insert_start:state_snapshot",
                        "cat": "python_probe",
                        "ph": "X",
                        "ts": 10,
                        "dur": 0,
                        "pid": 456,
                        "tid": 1,
                        "args": {
                            "domain": "python_probe",
                            "event_kind": "state_snapshot",
                            "model_input": False,
                            "source_event_name": "hicache_insert_start",
                            "target_id": "hiradix.insert",
                            "request_id": "req-p12",
                            "state_snapshot": {
                                "enabled": True,
                                "object_type": "HiRadixCache",
                                "object_id": "cache-456",
                                "derived": {
                                    "l1_resident_pages": [],
                                    "l2_resident_pages": [],
                                    "dirty_pages": [],
                                    "backuped_pages": [],
                                    "evicted_pages": [],
                                },
                            },
                        },
                    },
                    {
                        "name": "hicache_insert_end",
                        "cat": "python_probe",
                        "ph": "X",
                        "ts": 10,
                        "dur": 1,
                        "pid": 456,
                        "tid": 1,
                        "args": {
                            "domain": "python_probe",
                            "event_kind": "hicache_insert_end",
                            "model_input": True,
                            "event_role": "insert",
                            "request_id": "req-p12",
                            "tier_dst": "L1",
                            "page_identity": ["p1", "p2"],
                        },
                    },
                    {
                        "name": "hicache_insert_end:state_snapshot",
                        "cat": "python_probe",
                        "ph": "X",
                        "ts": 10,
                        "dur": 999,
                        "pid": 456,
                        "tid": 1,
                        "args": {
                            "domain": "python_probe",
                            "event_kind": "state_snapshot",
                            "model_input": False,
                            "source_event_name": "hicache_insert_end",
                            "target_id": "hiradix.insert",
                            "request_id": "req-p12",
                            "state_snapshot": {
                                "enabled": True,
                                "object_type": "HiRadixCache",
                                "object_id": "cache-456",
                                "derived": {
                                    "l1_resident_pages": ["p1", "p2"],
                                    "l2_resident_pages": [],
                                    "dirty_pages": ["p1", "p2"],
                                    "backuped_pages": [],
                                    "evicted_pages": [],
                                },
                                "capacity": {
                                    "page_size": 128,
                                    "write_policy": "write_back",
                                    "prefetch_policy": "best_effort",
                                    "l1_capacity_pages": 64,
                                    "l1_available_pages": 8,
                                    "l2_capacity_pages": 128,
                                    "l2_available_pages": 32,
                                    "prefetch_capacity_limit_pages": 48,
                                },
                            },
                        },
                    },
                    {
                        "name": "hicache_insert_start",
                        "cat": "python_probe",
                        "ph": "X",
                        "ts": 12,
                        "dur": 0,
                        "pid": 457,
                        "tid": 1,
                        "args": {
                            "domain": "python_probe",
                            "event_kind": "hicache_insert_start",
                            "model_input": True,
                            "event_role": "insert",
                            "tier_dst": "L1",
                            "page_identity": ["p3"],
                        },
                    },
                    {
                        "name": "hicache_insert_start:state_snapshot",
                        "cat": "python_probe",
                        "ph": "X",
                        "ts": 12,
                        "dur": 0,
                        "pid": 457,
                        "tid": 1,
                        "args": {
                            "domain": "python_probe",
                            "event_kind": "state_snapshot",
                            "model_input": False,
                            "source_event_name": "hicache_insert_start",
                            "target_id": "hiradix.insert",
                            "state_snapshot": {
                                "enabled": True,
                                "object_type": "HiRadixCache",
                                "object_id": "cache-457",
                                "derived": {
                                    "l1_resident_pages": [],
                                    "l2_resident_pages": [],
                                    "dirty_pages": [],
                                    "backuped_pages": [],
                                    "evicted_pages": [],
                                },
                            },
                        },
                    },
                    {
                        "name": "hicache_insert_end",
                        "cat": "python_probe",
                        "ph": "X",
                        "ts": 12,
                        "dur": 1,
                        "pid": 457,
                        "tid": 1,
                        "args": {
                            "domain": "python_probe",
                            "event_kind": "hicache_insert_end",
                            "model_input": True,
                            "event_role": "insert",
                            "tier_dst": "L1",
                            "page_identity": ["p3"],
                        },
                    },
                    {
                        "name": "hicache_insert_end:state_snapshot",
                        "cat": "python_probe",
                        "ph": "X",
                        "ts": 12,
                        "dur": 999,
                        "pid": 457,
                        "tid": 1,
                        "args": {
                            "domain": "python_probe",
                            "event_kind": "state_snapshot",
                            "model_input": False,
                            "source_event_name": "hicache_insert_end",
                            "target_id": "hiradix.insert",
                            "state_snapshot": {
                                "enabled": True,
                                "object_type": "HiRadixCache",
                                "object_id": "cache-457",
                                "derived": {
                                    "l1_resident_pages": ["p3"],
                                    "l2_resident_pages": [],
                                    "dirty_pages": ["p3"],
                                    "backuped_pages": [],
                                    "evicted_pages": [],
                                },
                            },
                        },
                    },
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    manifest = tmp / "state_validation/profile_manifest.json"
    manifest.write_text(
        json.dumps(
            {
                "run_dir": str(tmp / "state_validation"),
                "trace": {"torch_trace_files": [], "ld_preload_trace_files": []},
                "sidecar": {"python_probe_files": [{"path": str(sidecar), "exists": True}]},
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    config = tmp / "state_validation/modeling_cache_state.json"
    output_dir = tmp / "state_validation/modeling_out"
    config.write_text(
        json.dumps(
            {
                "input": {"profile_manifest": str(manifest)},
                "output_dir": str(output_dir),
                "mode": "cache_state",
                "validation": {
                    "faithful_replay_full_e2e_rel_error_max": 1000.0,
                    "hicache_state": {"enabled": True},
                },
                "cpp_model_config": {
                    "modules": ["hicache"],
                    "hicache": {
                        "enabled": True,
                        "page_size": 128,
                        "l1_capacity_pages": 64,
                        "l2_capacity_pages": 128,
                        "write_policy": "write_back",
                        "prefetch_policy": "best_effort",
                    },
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
            "--emit-validation",
        ],
        cwd=ROOT,
    )
    validation = json.loads((output_dir / "validation.json").read_text(encoding="utf-8"))
    assert validation["hicache_state"]["state_trace_ready"] is True, validation
    assert validation["hicache_state"]["final_state_match"] is True, validation
    assert validation["hicache_state"]["invariant_coverage_ready"] is True, validation
    assert validation["hicache_state"]["skipped_non_invariant_events"] == 0, validation
    state_trace_path = Path(validation["hicache_state"]["predicted_state_trace_path"])
    assert state_trace_path.is_file(), validation
    predicted_trace = json.loads(state_trace_path.read_text(encoding="utf-8"))
    assert predicted_trace["record_count"] > 0, predicted_trace
    assert predicted_trace["records"][0]["source_fact_id"].startswith("trace_event:"), predicted_trace
    request_coverage = validation["hicache_state"]["request_transition_coverage"]
    assert request_coverage["ready"] is True, validation
    assert request_coverage["requests_missing_oracle_snapshot"] == [], validation
    assert request_coverage["requests_missing_predicted_transition"] == [], validation
    assert request_coverage["predicted_transition_count_by_request"]["req-p12"] > 0, validation
    transition_coverage = validation["hicache_state"]["transition_coverage"]
    assert transition_coverage["ready"] is True, validation
    assert transition_coverage["predicted_transition_count"] == predicted_trace["record_count"], validation
    assert transition_coverage["predicted_transition_count_by_kind"]["add_l1_resident"] == 3, validation
    assert transition_coverage["predicted_transition_count_by_kind"]["mark_dirty"] == 3, validation
    assert transition_coverage["predicted_operation_count_by_kind"]["resident_state_update"] == 3, validation
    assert transition_coverage["predicted_operation_count_by_kind"]["page_metadata_update"] == 3, validation
    assert transition_coverage["predicted_transition_count_by_source_event"]["hicache_insert_end"] == 6, validation
    assert transition_coverage["pages_missing_predicted_transition"] == [], validation
    assert transition_coverage["pages_without_oracle_snapshot_evidence"] == [], validation
    event_delta = validation["hicache_state"]["event_delta_validation"]
    assert event_delta["ready"] is True, validation
    assert event_delta["comparable"] is True, validation
    assert event_delta["match"] is True, validation
    assert event_delta["shared_event_key_count"] == 2, validation
    assert event_delta["oracle_transition_count_by_kind"]["add_l1_resident"] == 3, validation
    assert event_delta["oracle_transition_count_by_kind"]["mark_dirty"] == 3, validation
    assert event_delta["predicted_comparable_transition_count_by_kind"]["add_l1_resident"] == 3, validation
    assert event_delta["predicted_comparable_transition_count_by_kind"]["mark_dirty"] == 3, validation
    assert event_delta["mismatch_count"] == 0, validation
    assert event_delta["mismatch_totals_by_kind"] == {}, validation
    timeline_delta = validation["hicache_state"]["timeline_delta_validation"]
    assert timeline_delta["ready"] is True, validation
    assert timeline_delta["match"] is True, validation
    assert timeline_delta["object_group_count"] == 2, validation
    assert timeline_delta["oracle_transition_count_by_kind"]["add_l1_resident"] == 3, validation
    assert timeline_delta["oracle_transition_count_by_kind"]["mark_dirty"] == 3, validation
    assert timeline_delta["predicted_transition_count_by_kind"]["add_l1_resident"] == 3, validation
    assert timeline_delta["predicted_transition_count_by_kind"]["mark_dirty"] == 3, validation
    assert timeline_delta["mismatch_count"] == 0, validation
    assert timeline_delta["mismatch_totals_by_kind"] == {}, validation
    capacity_summary = validation["hicache_state"]["oracle_capacity_summary"]
    assert capacity_summary["ready"] is True, validation
    assert capacity_summary["snapshot_count"] == 1, validation
    assert capacity_summary["unique_values"]["l1_capacity_pages"] == [64], validation
    assert capacity_summary["unique_values"]["l2_capacity_pages"] == [128], validation
    assert capacity_summary["unique_values"]["write_policy"] == ["write_back"], validation
    assert validation["hicache_state"]["oracle_observed_max_state_counts"]["l1_resident_pages"] == 3, validation
    assert validation["hicache_state"]["oracle_observed_max_state_counts"]["dirty_pages"] == 3, validation
    capacity_audit = validation["hicache_state"]["capacity_config_audit"]
    assert capacity_audit["oracle_capacity_ready"] is True, validation
    assert capacity_audit["target_config"]["l1_capacity_pages"] == 64, validation
    assert capacity_audit["comparisons"]["page_size"]["status"] == "match", validation
    assert capacity_audit["comparisons"]["write_policy"]["status"] == "match", validation
    assert capacity_audit["comparisons"]["prefetch_policy"]["status"] == "match", validation
    assert capacity_audit["comparisons"]["l1_capacity_pages"]["status"] == "matches_observed_pool", validation
    assert capacity_audit["comparisons"]["l2_capacity_pages"]["status"] == "matches_observed_pool", validation
    recommended = capacity_audit["recommended_target_config"]
    assert recommended["ready"] is True, validation
    assert recommended["hicache"]["page_size"] == 128, validation
    assert recommended["hicache"]["write_policy"] == "write_back", validation
    assert recommended["hicache"]["prefetch_policy"] == "best_effort", validation
    assert recommended["hicache"]["l1_capacity_pages"] == 64, validation
    assert recommended["hicache"]["l2_capacity_pages"] == 128, validation
    assert recommended["evidence"]["l1_capacity_pages"]["source"] == "explicit_target_config", validation
    assert recommended["evidence"]["l2_capacity_pages"]["source"] == "explicit_target_config", validation
    recommended_path = Path(validation["hicache_state"]["recommended_hicache_cpp_model_config_path"])
    assert recommended_path.is_file(), validation
    recommended_cpp = json.loads(recommended_path.read_text(encoding="utf-8"))
    assert recommended_cpp["modules"] == ["hicache"], recommended_cpp
    assert recommended_cpp["hicache"]["page_size"] == 128, recommended_cpp
    assert recommended_cpp["hicache"]["l1_capacity_pages"] == 64, recommended_cpp
    assert recommended_cpp["hicache"]["l2_capacity_pages"] == 128, recommended_cpp
    assert capacity_audit["likely_error_fields"] == [], validation
    assert validation["validation_ready"] is True, validation


def run_hicache_state_ignore_keys_fixture(tmp: Path) -> None:
    """验证 validation-only ignore_state_keys 只放宽声明字段。"""

    spec = importlib.util.spec_from_file_location(
        "trace_sim_model_runner_ignore_state_keys",
        ROOT / "scripts/internal/model_runner.py",
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[str(spec.name)] = module
    spec.loader.exec_module(module)

    module_summary = tmp / "ignore_state_keys/model_summary.json"
    oracle_trace = tmp / "ignore_state_keys/oracle_trace.json"
    oracle_trace_with_lock = tmp / "ignore_state_keys/oracle_trace_with_lock.json"
    module_summary.parent.mkdir(parents=True)
    module_summary.write_text(
        json.dumps(
            {
                "modules": [
                    {
                        "name": "HiCacheModule",
                        "hicache": {
                            "final_state": {
                                "l1_resident_pages": ["p1"],
                                "prefetch_ready_pages": ["model_prefetch"],
                            },
                            "transition_trace": [],
                            "missing_page_identity_events": 0,
                            "missing_invariant_facts": {},
                        },
                    }
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    oracle_trace.write_text(
        json.dumps(
            {
                "traceEvents": [
                    {
                        "name": "hicache_snapshot_end",
                        "cat": "python_probe",
                        "ph": "X",
                        "ts": 10,
                        "dur": 999,
                        "pid": 100,
                        "tid": 1,
                        "args": {
                            "domain": "python_probe",
                            "event_kind": "state_snapshot",
                            "model_input": False,
                            "source_event_name": "hicache_snapshot_end",
                            "state_snapshot": {
                                "enabled": True,
                                "derived": {
                                    "l1_resident_pages": ["p1"],
                                    "prefetch_ready_pages": ["oracle_prefetch"],
                                },
                            },
                        },
                    }
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    oracle_trace_with_lock.write_text(
        json.dumps(
            {
                "traceEvents": [
                    {
                        "name": "hicache_snapshot_end",
                        "cat": "python_probe",
                        "ph": "X",
                        "ts": 10,
                        "dur": 999,
                        "pid": 100,
                        "tid": 1,
                        "args": {
                            "domain": "python_probe",
                            "event_kind": "state_snapshot",
                            "model_input": False,
                            "source_event_name": "hicache_snapshot_end",
                            "state_snapshot": {
                                "enabled": True,
                                "derived": {
                                    "l1_resident_pages": ["p1"],
                                    "locked_pages": ["oracle_locked"],
                                    "prefetch_ready_pages": ["oracle_prefetch"],
                                },
                            },
                        },
                    }
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )

    ignored = module.build_hicache_state_validation_if_enabled(
        {"hicache_state": {"enabled": True, "ignore_state_keys": ["prefetch_ready_pages"]}},
        [oracle_trace],
        module_summary,
        None,
        [oracle_trace],
    )
    assert ignored is not None, ignored
    assert ignored["final_state_match"] is True, ignored
    assert ignored["sets_diff_by_tier"]["l1_resident_pages"]["match"] is True, ignored
    assert "prefetch_ready_pages" not in ignored["sets_diff_by_tier"], ignored
    assert ignored["ignored_sets_diff_by_tier"]["prefetch_ready_pages"]["match"] is False, ignored
    assert ignored["ignored_state_keys"] == ["prefetch_ready_pages"], ignored

    strict = module.build_hicache_state_validation_if_enabled(
        {"hicache_state": {"enabled": True}},
        [oracle_trace],
        module_summary,
        None,
        [oracle_trace],
    )
    assert strict is not None, strict
    assert strict["final_state_match"] is False, strict
    assert strict["first_mismatch"]["tier"] == "prefetch_ready_pages", strict

    oracle_only = module.build_hicache_state_validation_if_enabled(
        {"hicache_state": {"enabled": True, "ignore_state_keys": ["prefetch_ready_pages"]}},
        [oracle_trace_with_lock],
        module_summary,
        None,
        [oracle_trace_with_lock],
    )
    assert oracle_only is not None, oracle_only
    assert oracle_only["final_state_match"] is False, oracle_only
    assert oracle_only["sets_diff_by_tier"]["locked_pages"]["oracle_count"] == 1, oracle_only
    assert oracle_only["sets_diff_by_tier"]["locked_pages"]["model_count"] == 0, oracle_only


def run_hicache_lock_fact_oracle_fixture(tmp: Path) -> None:
    """验证 lock/ref final oracle 会用事实流修正尾部缺失 snapshot。"""

    sidecar = tmp / "lock_fact_oracle/trace/python_probe/python_probe_trace.rank0.pid458.json"
    sidecar.parent.mkdir(parents=True)
    sidecar.write_text(
        json.dumps(
            {
                "traceEvents": [
                    {
                        "name": "hicache_inc_lock_ref_end",
                        "cat": "python_probe",
                        "ph": "X",
                        "ts": 10,
                        "dur": 1,
                        "pid": 458,
                        "tid": 1,
                        "args": {
                            "domain": "python_probe",
                            "event_kind": "hicache_inc_lock_ref_end",
                            "model_input": True,
                            "event_role": "lock_ref_inc",
                            "page_identity": ["p_lock"],
                            "lock_delta": 1,
                        },
                    },
                    {
                        "name": "hicache_inc_lock_ref_end:state_snapshot",
                        "cat": "python_probe",
                        "ph": "X",
                        "ts": 10,
                        "dur": 1,
                        "pid": 458,
                        "tid": 1,
                        "args": {
                            "domain": "python_probe",
                            "event_kind": "state_snapshot",
                            "model_input": False,
                            "source_event_name": "hicache_inc_lock_ref_end",
                            "target_id": "hiradix.inc_lock_ref",
                            "state_snapshot": {
                                "enabled": True,
                                "object_type": "HiRadixCache",
                                "object_id": "cache-458",
                                "derived": {
                                    "l1_resident_pages": [],
                                    "l2_resident_pages": [],
                                    "dirty_pages": [],
                                    "backuped_pages": [],
                                    "evicted_pages": [],
                                    "locked_pages": ["p_lock"],
                                },
                            },
                        },
                    },
                    {
                        "name": "hicache_dec_lock_ref_end",
                        "cat": "python_probe",
                        "ph": "X",
                        "ts": 20,
                        "dur": 1,
                        "pid": 458,
                        "tid": 1,
                        "args": {
                            "domain": "python_probe",
                            "event_kind": "hicache_dec_lock_ref_end",
                            "model_input": True,
                            "event_role": "lock_ref_dec",
                            "page_identity": ["p_lock"],
                            "lock_delta": -1,
                        },
                    },
                    {
                        "name": "hicache_dec_lock_ref_start:state_snapshot",
                        "cat": "python_probe",
                        "ph": "X",
                        "ts": 20,
                        "dur": 0,
                        "pid": 458,
                        "tid": 1,
                        "args": {
                            "domain": "python_probe",
                            "event_kind": "state_snapshot",
                            "model_input": False,
                            "source_event_name": "hicache_dec_lock_ref_start",
                            "target_id": "hiradix.dec_lock_ref",
                            "state_snapshot": {
                                "enabled": True,
                                "object_type": "HiRadixCache",
                                "object_id": "cache-458",
                                "derived": {
                                    "l1_resident_pages": [],
                                    "l2_resident_pages": [],
                                    "dirty_pages": [],
                                    "backuped_pages": [],
                                    "evicted_pages": [],
                                    "locked_pages": ["p_lock"],
                                },
                            },
                        },
                    },
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    manifest = tmp / "lock_fact_oracle/profile_manifest.json"
    manifest.write_text(
        json.dumps(
            {
                "run_dir": str(tmp / "lock_fact_oracle"),
                "trace": {"torch_trace_files": [], "ld_preload_trace_files": []},
                "sidecar": {"python_probe_files": [{"path": str(sidecar), "exists": True}]},
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    config = tmp / "lock_fact_oracle/modeling_cache_state.json"
    output_dir = tmp / "lock_fact_oracle/modeling_out"
    config.write_text(
        json.dumps(
            {
                "input": {"profile_manifest": str(manifest)},
                "output_dir": str(output_dir),
                "mode": "cache_state",
                "validation": {
                    "faithful_replay_full_e2e_rel_error_max": 1000.0,
                    "hicache_state": {"enabled": True},
                },
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
            "--emit-validation",
        ],
        cwd=ROOT,
    )
    validation = json.loads((output_dir / "validation.json").read_text(encoding="utf-8"))
    hicache_state = validation["hicache_state"]
    assert hicache_state["final_state_match"] is True, validation
    assert hicache_state["oracle_final_state_counts"]["locked_pages"] == 0, validation
    assert hicache_state["model_final_state_counts"]["locked_pages"] == 0, validation
    assert hicache_state["lock_fact_oracle"]["ready"] is True, validation
    assert hicache_state["lock_fact_oracle"]["lock_fact_event_count"] == 2, validation
    assert hicache_state["skipped_non_invariant_events"] == 2, validation
    assert hicache_state["model_transition_events"] == 0, validation
    assert hicache_state["invariant_coverage_ready"] is True, validation
    assert hicache_state["missing_invariant_facts"] == [], validation
    assert hicache_state["non_invariant_fact_usage_by_role"] == {}, validation
    assert validation["validation_ready"] is True, validation
    assert validation["validation_errors"] == [], validation


def run_hicache_dirty_derivation_fixture() -> None:
    """验证 snapshot 没有可靠 dirty 字段时按未备份 L1 页派生 dirty。"""

    spec = importlib.util.spec_from_file_location(
        "trace_sim_model_runner_dirty_derivation",
        ROOT / "scripts/internal/model_runner.py",
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[str(spec.name)] = module
    spec.loader.exec_module(module)

    derived = module.derived_hicache_state_from_snapshot(
        {
            "enabled": True,
            "nodes": [
                {
                    "hash_value": ["p_dirty"],
                    "has_device_value": True,
                    "has_host_value": False,
                    "backuped": False,
                    "dirty": False,
                    "evicted": False,
                },
                {
                    "hash_value": ["p_clean"],
                    "has_device_value": True,
                    "has_host_value": True,
                    "backuped": True,
                    "dirty": False,
                    "evicted": False,
                },
            ],
        }
    )
    assert derived["l1_resident_pages"] == ["p_clean", "p_dirty"], derived
    assert derived["l2_resident_pages"] == ["p_clean"], derived
    assert derived["backuped_pages"] == ["p_clean"], derived
    assert derived["dirty_pages"] == ["p_dirty"], derived


def run_hicache_capacity_config_audit_fixture() -> None:
    """验证 capacity config audit 能区分有效 budget 和明显错误。"""

    spec = importlib.util.spec_from_file_location(
        "trace_sim_model_runner_capacity_audit",
        ROOT / "scripts/internal/model_runner.py",
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[str(spec.name)] = module
    spec.loader.exec_module(module)

    audit = module.build_hicache_capacity_config_audit(
        {
            "ready": True,
            "unique_values": {
                "page_size": [128],
                "write_policy": ["write_back"],
                "prefetch_policy": ["timeout"],
                "l1_capacity_pages": [48],
                "l2_capacity_pages": [96],
            },
        },
        {
            "page_size": 128,
            "write_policy": "write_back",
            "prefetch_policy": "timeout",
            "l1_capacity_pages": 46,
            "l2_capacity_pages": 128,
        },
        {
            "l1_resident_pages": 46,
            "l2_resident_pages": 88,
        },
        {
            "l1_resident_pages": 46,
            "l2_resident_pages": 100,
        },
    )

    assert audit["comparisons"]["l1_capacity_pages"]["status"] == "target_below_observed_pool", audit
    assert audit["comparisons"]["l1_capacity_pages"]["final_count_status"] == "matches_oracle_final_count", audit
    assert audit["comparisons"]["l1_capacity_pages"]["observed_max_status"] == "matches_oracle_observed_max_count", audit
    assert audit["comparisons"]["l2_capacity_pages"]["status"] == "target_exceeds_observed_pool", audit
    assert audit["comparisons"]["l2_capacity_pages"]["observed_max_status"] == "target_above_oracle_observed_max_count", audit
    assert "l1_capacity_pages" in audit["warning_fields"], audit
    assert "l2_capacity_pages" in audit["likely_error_fields"], audit
    recommended = audit["recommended_target_config"]
    assert recommended["ready"] is True, audit
    assert recommended["hicache"]["l1_capacity_pages"] == 46, audit
    assert recommended["hicache"]["l2_capacity_pages"] == 128, audit
    assert recommended["evidence"]["l1_capacity_pages"]["source"] == "explicit_target_config", audit
    assert recommended["evidence"]["l2_capacity_pages"]["source"] == "explicit_target_config", audit

    no_capacity_audit = module.build_hicache_capacity_config_audit(
        {
            "ready": True,
            "unique_values": {
                "page_size": [128],
                "write_policy": ["write_through"],
                "prefetch_policy": ["timeout"],
                "l1_capacity_pages": [64],
                "l2_capacity_pages": [129],
            },
        },
        {
            "page_size": 128,
            "write_policy": "write_through",
            "prefetch_policy": "timeout",
        },
        {
            "l1_resident_pages": 56,
            "l2_resident_pages": 121,
        },
        {
            "l1_resident_pages": 62,
            "l2_resident_pages": 128,
        },
    )
    no_capacity_recommended = no_capacity_audit["recommended_target_config"]
    assert no_capacity_recommended["ready"] is True, no_capacity_audit
    assert no_capacity_recommended["hicache"]["page_size"] == 128, no_capacity_audit
    assert "l1_capacity_pages" not in no_capacity_recommended["hicache"], no_capacity_audit
    assert "l2_capacity_pages" not in no_capacity_recommended["hicache"], no_capacity_audit
    assert no_capacity_recommended["evidence"]["l1_capacity_pages"]["source"] == "not_auto_recommended", no_capacity_audit
    assert no_capacity_recommended["evidence"]["l2_capacity_pages"]["source"] == "not_auto_recommended", no_capacity_audit


def run_hicache_target_experiment_config_fixture(tmp: Path) -> None:
    """验证 target 实验配置能生成 C++ HiCache config，但不会粗算 capacity。"""

    spec = importlib.util.spec_from_file_location(
        "trace_sim_model_runner_target_experiment",
        ROOT / "scripts/internal/model_runner.py",
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[str(spec.name)] = module
    spec.loader.exec_module(module)

    experiment = tmp / "target_experiment.json"
    experiment.write_text(
        json.dumps(
            {
                "server": {
                    "command": [
                        "python3",
                        "-m",
                        "sglang.launch_server",
                        "--page-size",
                        "64",
                        "--max-total-tokens",
                        "6144",
                        "--hicache-ratio",
                        "2.0",
                        "--hicache-write-policy",
                        "write_back",
                        "--hicache-storage-backend-extra-config",
                        json.dumps(
                            {
                                "prefetch_threshold": 128,
                                "prefetch_timeout_base": 1.5,
                                "prefetch_timeout_per_ki_token": 0.25,
                                "prefetch_timeout_max": 3.0,
                            }
                        ),
                        "--hicache-storage-prefetch-policy",
                        "timeout",
                    ]
                },
                "modeling": {
                    "hicache": {
                        "l1_capacity_pages": 46,
                        "l2_capacity_pages": 88,
                    }
                },
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    output_dir = tmp / "target_experiment_modeling"
    generated_path = module.write_cpp_model_config(
        {
            "input": {"target_experiment_config": str(experiment)},
            "modules": [
                {
                    "name": "HiCacheModule",
                    "enabled": True,
                    "config": {"hicache": {"enabled": True}},
                }
            ],
        },
        output_dir,
        "cache_state",
    )
    assert generated_path is not None and generated_path.is_file(), generated_path
    generated = json.loads(generated_path.read_text(encoding="utf-8"))
    assert generated["modules"] == ["hicache"], generated
    hicache = generated["hicache"]
    assert hicache["page_size"] == 64, generated
    assert hicache["write_policy"] == "write_back", generated
    assert hicache["prefetch_policy"] == "timeout", generated
    assert hicache["l1_capacity_pages"] == 46, generated
    assert hicache["l2_capacity_pages"] == 88, generated
    assert hicache["prefetch_timeout_base_sec"] == 1.5, generated
    assert hicache["prefetch_timeout_per_ki_token_sec"] == 0.25, generated
    assert hicache["prefetch_timeout_max_sec"] == 3.0, generated

    no_capacity_output = tmp / "target_experiment_no_capacity_modeling"
    no_capacity_path = module.write_cpp_model_config(
        {
            "input": {
                "target_experiment_config": {
                    "server": {
                        "command": [
                            "python3",
                            "-m",
                            "sglang.launch_server",
                            "--max-total-tokens",
                            "6144",
                            "--hicache-ratio",
                            "2.0",
                            "--hicache-write-policy",
                            "write_through",
                        ]
                    }
                }
            }
        },
        no_capacity_output,
        "cache_state",
    )
    assert no_capacity_path is not None and no_capacity_path.is_file(), no_capacity_path
    no_capacity = json.loads(no_capacity_path.read_text(encoding="utf-8"))
    assert no_capacity["hicache"]["write_policy"] == "write_through", no_capacity
    assert "l1_capacity_pages" not in no_capacity["hicache"], no_capacity
    assert "l2_capacity_pages" not in no_capacity["hicache"], no_capacity
    assert "page_size" not in no_capacity["hicache"], no_capacity


def run_hicache_observed_policy_rejected_fixture() -> None:
    """验证 Python runner 不再兼容旧 observed policy 配置。"""

    spec = importlib.util.spec_from_file_location(
        "trace_sim_model_runner_observed_policy_rejected",
        ROOT / "scripts/internal/model_runner.py",
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[str(spec.name)] = module
    spec.loader.exec_module(module)

    invalid_module_configs = [
        {"modules": [{"name": "HiCacheModule", "config": {"write_policy": "observed"}}]},
        {"modules": [{"name": "hicache", "config": {"prefetch_policy": "observed"}}]},
        {"modules": [{"name": "hicache", "config": {"storage_prefetch_policy": "observed"}}]},
    ]
    for config in invalid_module_configs:
        try:
            module.hicache_config_from_modules(config)
            raise AssertionError(config)
        except ValueError as exc:
            assert "observed" in str(exc), exc

    invalid_target_configs = [
        {"input": {"target_experiment_config": {"server": {"command": ["python", "-m", "sglang", "--hicache-write-policy", "observed"]}}}},
        {
            "input": {
                "target_experiment_config": {
                    "server": {"command": ["python", "-m", "sglang"]},
                    "modeling": {"hicache": {"prefetch_policy": "observed"}},
                }
            }
        },
    ]
    for config in invalid_target_configs:
        try:
            module.hicache_config_from_target_experiment(config)
            raise AssertionError(config)
        except ValueError as exc:
            assert "observed" in str(exc), exc


def run_hicache_l3_evidence_only_fixture(tmp: Path) -> None:
    """验证未绑定 target schedule 的 L3->L2 movement 不直接驱动 target state。"""

    source = tmp / "l3_evidence/source/python_probe_trace.rank0.pid791.json"
    oracle = tmp / "l3_evidence/oracle/python_probe_trace.rank0.pid791.json"
    source.parent.mkdir(parents=True)
    oracle.parent.mkdir(parents=True)
    source.write_text(
        json.dumps(
            {
                "traceEvents": [
                    {
                        "name": "hicache_l3_l2_transfer_end",
                        "cat": "python_probe",
                        "ph": "X",
                        "ts": 10,
                        "dur": 1,
                        "pid": 791,
                        "tid": 1,
                        "args": {
                            "domain": "python_probe",
                            "model_input": True,
                            "event_role": "l3_to_l2_transfer",
                            "tier_src": "L3",
                            "tier_dst": "L2",
                            "request_id": "req-l3",
                            "page_identity": "p_l3",
                        },
                    }
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    oracle.write_text(
        json.dumps(
            {
                "traceEvents": [
                    {
                        "name": "hicache_l3_l2_transfer_end:state_snapshot",
                        "cat": "python_probe",
                        "ph": "X",
                        "ts": 11,
                        "dur": 999,
                        "pid": 791,
                        "tid": 1,
                        "args": {
                            "domain": "python_probe",
                            "event_kind": "state_snapshot",
                            "model_input": False,
                            "request_id": "req-l3",
                            "state_snapshot": {
                                "enabled": True,
                                "derived": {
                                    "l1_resident_pages": [],
                                    "l2_resident_pages": ["p_l3"],
                                    "dirty_pages": [],
                                    "backuped_pages": ["p_l3"],
                                    "evicted_pages": [],
                                },
                            },
                        },
                    }
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    manifest = tmp / "l3_evidence/profile_manifest.json"
    manifest.write_text(
        json.dumps(
            {
                "run_dir": str(tmp / "l3_evidence"),
                "trace": {"torch_trace_files": [], "ld_preload_trace_files": []},
                "sidecar": {"python_probe_files": [{"path": str(source), "exists": True}]},
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    config = tmp / "l3_evidence/modeling_cache_state.json"
    output_dir = tmp / "l3_evidence/modeling_out"
    config.write_text(
        json.dumps(
            {
                "input": {"profile_manifest": str(manifest)},
                "output_dir": str(output_dir),
                "mode": "cache_state",
                "validation": {"hicache_state": {"enabled": True, "oracle_trace_paths": [str(oracle)]}},
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
            "--emit-validation",
        ],
        cwd=ROOT,
    )
    validation = json.loads((output_dir / "validation.json").read_text(encoding="utf-8"))
    module_summary = json.loads((output_dir / "model_summary.json").read_text(encoding="utf-8"))
    final_state = module_summary["modules"][0]["hicache"]["final_state"]
    assert final_state["l2_resident_pages"] == [], final_state
    assert final_state["l3_resident_pages"] == [], final_state
    assert final_state["backuped_pages"] == [], final_state
    assert module_summary["modules"][0]["hicache"]["skipped_non_invariant_events"] == 1, module_summary
    assert "l3_resident_pages" not in validation["hicache_state"]["sets_diff_by_tier"], validation
    assert validation["hicache_state"]["final_state_match"] is False, validation
    assert validation["hicache_state"]["invariant_coverage_ready"] is True, validation
    assert validation["hicache_state"]["missing_invariant_facts"] == [], validation
    assert validation["hicache_state"]["non_invariant_fact_usage_by_role"] == {}, validation
    assert validation["validation_ready"] is False, validation
    assert validation["validation_errors"] == ["hicache_final_state_mismatch"], validation


def run_hicache_state_prediction_fixture(tmp: Path) -> None:
    """验证 source trace 和独立 oracle trace 可以走同一套 state diff。"""

    source = tmp / "state_prediction/base/python_probe_trace.rank0.pid789.json"
    oracle = tmp / "state_prediction/target/oracle_state.rank0.pid789.json"
    source.parent.mkdir(parents=True)
    oracle.parent.mkdir(parents=True)
    source.write_text(
        json.dumps(
            {
                "traceEvents": [
                    {
                        "name": "hicache_insert_end",
                        "cat": "python_probe",
                        "ph": "X",
                        "ts": 10,
                        "dur": 1,
                        "pid": 789,
                        "tid": 1,
                        "args": {
                            "domain": "python_probe",
                            "model_input": True,
                            "event_role": "insert",
                            "tier_dst": "L1",
                            "page_identity": ["p3"],
                        },
                    },
                    {
                        "name": "hicache_write_backup_end",
                        "cat": "python_probe",
                        "ph": "X",
                        "ts": 20,
                        "dur": 1,
                        "pid": 789,
                        "tid": 1,
                        "args": {
                            "domain": "python_probe",
                            "model_input": True,
                            "event_role": "write_backup",
                            "tier_src": "L1",
                            "tier_dst": "L2",
                            "page_identity": "p3",
                        },
                    },
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    oracle.write_text(
        json.dumps(
            {
                "traceEvents": [
                    {
                        "name": "hicache_write_backup_end:state_snapshot",
                        "cat": "python_probe",
                        "ph": "X",
                        "ts": 21,
                        "dur": 999,
                        "pid": 789,
                        "tid": 1,
                        "args": {
                            "domain": "python_probe",
                            "event_kind": "state_snapshot",
                            "model_input": False,
                            "state_snapshot": {
                                "enabled": True,
                                "derived": {
                                    "l1_resident_pages": ["p3"],
                                    "l2_resident_pages": ["p3"],
                                    "dirty_pages": [],
                                    "backuped_pages": ["p3"],
                                    "evicted_pages": [],
                                },
                            },
                        },
                    }
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    manifest = tmp / "state_prediction/profile_manifest.json"
    manifest.write_text(
        json.dumps(
            {
                "run_dir": str(tmp / "state_prediction"),
                "trace": {"torch_trace_files": [], "ld_preload_trace_files": []},
                "sidecar": {"python_probe_files": [{"path": str(source), "exists": True}]},
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    config = tmp / "state_prediction/modeling_cache_state_prediction.json"
    output_dir = tmp / "state_prediction/modeling_out"
    config.write_text(
        json.dumps(
            {
                "input": {"profile_manifest": str(manifest)},
                "output_dir": str(output_dir),
                "mode": "cache_state",
                "validation": {
                    "hicache_state": {
                        "enabled": True,
                        "oracle_trace_paths": [str(oracle)],
                    },
                },
                "cpp_model_config": {
                    "modules": ["hicache"],
                    "hicache": {"enabled": True, "write_policy": "write_through"},
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
            "--emit-validation",
        ],
        cwd=ROOT,
    )
    validation = json.loads((output_dir / "validation.json").read_text(encoding="utf-8"))
    assert validation["hicache_state"]["oracle_trace_files"] == [str(oracle)], validation
    assert validation["hicache_state"]["final_state_match"] is True, validation
    assert validation["hicache_state"]["predicted_state_trace_ready"] is True, validation
    assert validation["hicache_state"]["non_invariant_fact_usage"] == [], validation
    assert validation["validation_ready"] is True, validation

    override_output_dir = tmp / "state_prediction/modeling_override_out"
    override_config = tmp / "state_prediction/modeling_cache_state_prediction_override.json"
    override_config.write_text(
        json.dumps(
            {
                "input": {"profile_manifest": str(manifest)},
                "output_dir": str(override_output_dir),
                "mode": "cache_state",
                "validation": {"hicache_state": {"enabled": True}},
                "cpp_model_config": {
                    "modules": ["hicache"],
                    "hicache": {"enabled": True, "write_policy": "write_through"},
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
            str(override_config),
            "--hicache-oracle-trace",
            str(oracle),
            "--emit-validation",
        ],
        cwd=ROOT,
    )
    override_validation = json.loads((override_output_dir / "validation.json").read_text(encoding="utf-8"))
    assert override_validation["hicache_state"]["oracle_trace_files"] == [str(oracle)], override_validation
    assert override_validation["validation_ready"] is True, override_validation


def run_hicache_prefetch_oracle_fixture(tmp: Path) -> None:
    """验证 prefetch_progress_state 可作为 ready pages 的 validation oracle。"""

    source = tmp / "prefetch_oracle/base/python_probe_trace.rank0.pid801.json"
    oracle = tmp / "prefetch_oracle/target/oracle_trace.rank0.pid801.json"
    source.parent.mkdir(parents=True)
    oracle.parent.mkdir(parents=True)
    source_events = [
        {
            "name": "hicache_prefetch_schedule_end",
            "cat": "python_probe",
            "ph": "X",
            "ts": 10,
            "dur": 1,
            "pid": 801,
            "tid": 1,
            "args": {
                "domain": "python_probe",
                "model_input": True,
                "event_role": "prefetch_schedule",
                "page_identity": ["p1", "p2"],
            },
        },
        {
            "name": "hicache_prefetch_progress_start",
            "cat": "python_probe",
            "ph": "X",
            "ts": 11,
            "dur": 1,
            "pid": 801,
            "tid": 1,
            "args": {
                "domain": "python_probe",
                "model_input": True,
                "event_role": "prefetch_progress",
                "request_id": "req-prefetch",
                "prefetch_progress_state": {
                    "request_id": "req-prefetch",
                    "page_size": 64,
                    "operation_hash_pages": ["p1", "p2"],
                    "completed_tokens": 128,
                    "check_return": None,
                },
            },
        },
        {
            "name": "hicache_prefetch_progress_end",
            "cat": "python_probe",
            "ph": "X",
            "ts": 12,
            "dur": 1,
            "pid": 801,
            "tid": 1,
            "args": {
                "domain": "python_probe",
                "model_input": True,
                "event_role": "prefetch_progress",
                "request_id": "req-prefetch",
                "prefetch_done": True,
                "prefetch_progress_state": {
                    "request_id": "req-prefetch",
                    "page_size": 64,
                    "check_return": True,
                },
            },
        },
    ]
    source.write_text(
        json.dumps({"traceEvents": source_events}, ensure_ascii=False),
        encoding="utf-8",
    )
    oracle.write_text(
        json.dumps(
            {
                "traceEvents": source_events
                + [
                    {
                        "name": "hicache_prefetch_progress_end:state_snapshot",
                        "cat": "python_probe",
                        "ph": "X",
                        "ts": 13,
                        "dur": 1,
                        "pid": 801,
                        "tid": 1,
                        "args": {
                            "domain": "python_probe",
                            "event_kind": "state_snapshot",
                            "model_input": False,
                            "state_snapshot": {
                                "enabled": True,
                                "derived": {
                                    "l1_resident_pages": [],
                                    "l2_resident_pages": ["p1", "p2"],
                                    "dirty_pages": [],
                                    "backuped_pages": ["p1", "p2"],
                                    "evicted_pages": [],
                                },
                            },
                        },
                    },
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    manifest = tmp / "prefetch_oracle/profile_manifest.json"
    manifest.write_text(
        json.dumps(
            {
                "run_dir": str(tmp / "prefetch_oracle"),
                "trace": {"torch_trace_files": [], "ld_preload_trace_files": []},
                "sidecar": {"python_probe_files": [{"path": str(source), "exists": True}]},
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    config = tmp / "prefetch_oracle/modeling_cache_state_prediction.json"
    output_dir = tmp / "prefetch_oracle/modeling_out"
    config.write_text(
        json.dumps(
            {
                "input": {"profile_manifest": str(manifest)},
                "output_dir": str(output_dir),
                "mode": "cache_state",
                "validation": {"hicache_state": {"enabled": True, "oracle_trace_paths": [str(oracle)]}},
                "cpp_model_config": {
                    "modules": ["hicache"],
                    "hicache": {"enabled": True, "prefetch_policy": "wait_complete"},
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
            "--emit-validation",
        ],
        cwd=ROOT,
    )
    validation = json.loads((output_dir / "validation.json").read_text(encoding="utf-8"))
    hicache = validation["hicache_state"]
    assert hicache["final_state_match"] is True, validation
    assert hicache["sets_diff_by_tier"]["prefetch_planned_pages"]["match"] is True, validation
    assert hicache["sets_diff_by_tier"]["prefetch_ready_pages"]["match"] is True, validation
    assert "prefetch_ready_pages" not in hicache["unchecked_model_state_keys"], validation


def run_hicache_prefetch_transfer_oracle_fixture(tmp: Path) -> None:
    """验证 l3_to_l2_transfer_end 可作为 prefetch ready pages 的 oracle evidence。"""

    spec = importlib.util.spec_from_file_location(
        "trace_sim_model_runner_prefetch_transfer",
        ROOT / "scripts/internal/model_runner.py",
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[str(spec.name)] = module
    spec.loader.exec_module(module)

    trace = tmp / "prefetch_transfer_oracle/trace.json"
    trace.parent.mkdir(parents=True)
    trace.write_text(
        json.dumps(
            {
                "traceEvents": [
                    {
                        "name": "hicache_prefetch_schedule_end",
                        "cat": "python_probe",
                        "ph": "X",
                        "ts": 10,
                        "dur": 1,
                        "pid": 804,
                        "tid": 1,
                        "args": {
                            "domain": "python_probe",
                            "model_input": True,
                            "event_role": "prefetch_schedule",
                            "request_id": "req-transfer",
                            "page_identity": ["p1", "p2"],
                        },
                    },
                    {
                        "name": "hicache_l3_l2_transfer_end",
                        "cat": "python_probe",
                        "ph": "X",
                        "ts": 11,
                        "dur": 1,
                        "pid": 804,
                        "tid": 1,
                        "args": {
                            "domain": "python_probe",
                            "model_input": True,
                            "event_role": "l3_to_l2_transfer",
                            "request_id": "req-transfer",
                            "page_identity": ["p1", "p2"],
                            "completed_tokens": 64,
                            "page_size": 64,
                        },
                    },
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    oracle = module.extract_hicache_prefetch_oracle_state([trace])
    assert oracle["prefetch_planned_pages"] == ["p1", "p2"], oracle
    assert oracle["prefetch_ready_pages"] == ["p1"], oracle
    assert "prefetch_late_pages" not in oracle, oracle
    assert "prefetch_suppressed_pages" not in oracle, oracle


def run_hicache_prefetch_timeout_oracle_fixture(tmp: Path) -> None:
    """验证 timeout progress oracle 会同时校验 ready 和 late pages。"""

    source = tmp / "prefetch_timeout_oracle/base/python_probe_trace.rank0.pid802.json"
    oracle = tmp / "prefetch_timeout_oracle/target/oracle_trace.rank0.pid802.json"
    source.parent.mkdir(parents=True)
    oracle.parent.mkdir(parents=True)
    events = [
        {
            "name": "hicache_prefetch_schedule_end",
            "cat": "python_probe",
            "ph": "X",
            "ts": 10,
            "dur": 1,
            "pid": 802,
            "tid": 1,
            "args": {
                "domain": "python_probe",
                "model_input": True,
                "event_role": "prefetch_schedule",
                "request_id": "req-timeout",
                "page_identity": ["p1", "p2"],
            },
        },
        {
            "name": "hicache_prefetch_progress_start",
            "cat": "python_probe",
            "ph": "X",
            "ts": 11,
            "dur": 1,
            "pid": 802,
            "tid": 1,
            "args": {
                "domain": "python_probe",
                "model_input": True,
                "event_role": "prefetch_progress",
                "request_id": "req-timeout",
                "prefetch_done": True,
                "prefetch_progress_state": {
                    "request_id": "req-timeout",
                    "page_size": 64,
                    "operation_hash_pages": ["p1", "p2"],
                    "completed_tokens": 64,
                    "check_return": None,
                    "has_ongoing_prefetch": True,
                },
            },
        },
        {
            "name": "hicache_prefetch_progress_end",
            "cat": "python_probe",
            "ph": "X",
            "ts": 12,
            "dur": 1,
            "pid": 802,
            "tid": 1,
            "args": {
                "domain": "python_probe",
                "model_input": True,
                "event_role": "prefetch_progress",
                "request_id": "req-timeout",
                "prefetch_progress_state": {
                    "request_id": "req-timeout",
                    "page_size": 64,
                    "check_return": True,
                    "has_ongoing_prefetch": False,
                },
            },
        },
    ]
    source.write_text(json.dumps({"traceEvents": events}, ensure_ascii=False), encoding="utf-8")
    oracle.write_text(
        json.dumps(
            {
                "traceEvents": events
                + [
                    {
                        "name": "hicache_prefetch_progress_end:state_snapshot",
                        "cat": "python_probe",
                        "ph": "X",
                        "ts": 13,
                        "dur": 1,
                        "pid": 802,
                        "tid": 1,
                        "args": {
                            "domain": "python_probe",
                            "event_kind": "state_snapshot",
                            "model_input": False,
                            "state_snapshot": {
                                "enabled": True,
                                "derived": {
                                    "l1_resident_pages": [],
                                    "l2_resident_pages": ["p1"],
                                    "dirty_pages": [],
                                    "backuped_pages": ["p1"],
                                    "evicted_pages": [],
                                },
                            },
                        },
                    }
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    manifest = tmp / "prefetch_timeout_oracle/profile_manifest.json"
    manifest.write_text(
        json.dumps(
            {
                "run_dir": str(tmp / "prefetch_timeout_oracle"),
                "trace": {"torch_trace_files": [], "ld_preload_trace_files": []},
                "sidecar": {"python_probe_files": [{"path": str(source), "exists": True}]},
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    config = tmp / "prefetch_timeout_oracle/modeling_cache_state_prediction.json"
    output_dir = tmp / "prefetch_timeout_oracle/modeling_out"
    config.write_text(
        json.dumps(
            {
                "input": {"profile_manifest": str(manifest)},
                "output_dir": str(output_dir),
                "mode": "cache_state",
                "validation": {"hicache_state": {"enabled": True, "oracle_trace_paths": [str(oracle)]}},
                "cpp_model_config": {
                    "modules": ["hicache"],
                    "hicache": {"enabled": True, "prefetch_policy": "timeout"},
                },
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    subprocess.check_call([sys.executable, str(ROOT / "scripts/internal/model_runner.py"), "--config", str(config), "--emit-validation"], cwd=ROOT)
    validation = json.loads((output_dir / "validation.json").read_text(encoding="utf-8"))
    hicache = validation["hicache_state"]
    assert hicache["final_state_match"] is True, validation
    assert hicache["sets_diff_by_tier"]["prefetch_ready_pages"]["match"] is True, validation
    assert hicache["sets_diff_by_tier"]["prefetch_late_pages"]["match"] is True, validation
    assert hicache["model_final_state_counts"]["prefetch_ready_pages"] == 1, validation
    assert hicache["model_final_state_counts"]["prefetch_late_pages"] == 1, validation
    assert "prefetch_late_pages" not in hicache["unchecked_model_state_keys"], validation


def run_hicache_prefetch_suppressed_oracle_fixture(tmp: Path) -> None:
    """验证没有 operation progress 的 prefetch check 会校验 suppressed pages。"""

    source = tmp / "prefetch_suppressed_oracle/base/python_probe_trace.rank0.pid803.json"
    oracle = tmp / "prefetch_suppressed_oracle/target/oracle_trace.rank0.pid803.json"
    source.parent.mkdir(parents=True)
    oracle.parent.mkdir(parents=True)
    events = [
        {
            "name": "hicache_prefetch_schedule_end",
            "cat": "python_probe",
            "ph": "X",
            "ts": 10,
            "dur": 1,
            "pid": 803,
            "tid": 1,
            "args": {
                "domain": "python_probe",
                "model_input": True,
                "event_role": "prefetch_schedule",
                "request_id": "req-suppressed",
                "page_identity": ["p1", "p2"],
            },
        },
        {
            "name": "hicache_prefetch_progress_end",
            "cat": "python_probe",
            "ph": "X",
            "ts": 20,
            "dur": 1,
            "pid": 803,
            "tid": 1,
            "args": {
                "domain": "python_probe",
                "model_input": True,
                "event_role": "prefetch_progress",
                "request_id": "req-suppressed",
                "prefetch_done": True,
                "prefetch_progress_state": {
                    "request_id": "req-suppressed",
                    "page_size": 64,
                    "check_return": True,
                    "has_ongoing_prefetch": False,
                },
            },
        },
    ]
    source.write_text(json.dumps({"traceEvents": events}, ensure_ascii=False), encoding="utf-8")
    oracle.write_text(
        json.dumps(
            {
                "traceEvents": events
                + [
                    {
                        "name": "hicache_prefetch_progress_end:state_snapshot",
                        "cat": "python_probe",
                        "ph": "X",
                        "ts": 21,
                        "dur": 1,
                        "pid": 803,
                        "tid": 1,
                        "args": {
                            "domain": "python_probe",
                            "event_kind": "state_snapshot",
                            "model_input": False,
                            "state_snapshot": {
                                "enabled": True,
                                "derived": {
                                    "l1_resident_pages": [],
                                    "l2_resident_pages": [],
                                    "dirty_pages": [],
                                    "backuped_pages": [],
                                    "evicted_pages": [],
                                },
                            },
                        },
                    }
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    manifest = tmp / "prefetch_suppressed_oracle/profile_manifest.json"
    manifest.write_text(
        json.dumps(
            {
                "run_dir": str(tmp / "prefetch_suppressed_oracle"),
                "trace": {"torch_trace_files": [], "ld_preload_trace_files": []},
                "sidecar": {"python_probe_files": [{"path": str(source), "exists": True}]},
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    config = tmp / "prefetch_suppressed_oracle/modeling_cache_state_prediction.json"
    output_dir = tmp / "prefetch_suppressed_oracle/modeling_out"
    config.write_text(
        json.dumps(
            {
                "input": {"profile_manifest": str(manifest)},
                "output_dir": str(output_dir),
                "mode": "cache_state",
                "validation": {"hicache_state": {"enabled": True, "oracle_trace_paths": [str(oracle)]}},
                "cpp_model_config": {
                    "modules": ["hicache"],
                    "hicache": {"enabled": True, "prefetch_policy": "best_effort"},
                },
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    subprocess.check_call([sys.executable, str(ROOT / "scripts/internal/model_runner.py"), "--config", str(config), "--emit-validation"], cwd=ROOT)
    validation = json.loads((output_dir / "validation.json").read_text(encoding="utf-8"))
    hicache = validation["hicache_state"]
    assert hicache["final_state_match"] is True, validation
    assert hicache["sets_diff_by_tier"]["prefetch_suppressed_pages"]["match"] is True, validation
    assert hicache["model_final_state_counts"]["prefetch_suppressed_pages"] == 2, validation
    assert "prefetch_suppressed_pages" not in hicache["unchecked_model_state_keys"], validation


def run_hicache_state_mismatch_fixture(tmp: Path) -> None:
    """验证 mismatch 输出能指向候选 state transition。"""

    source = tmp / "state_mismatch/base/python_probe_trace.rank0.pid790.json"
    oracle = tmp / "state_mismatch/target/oracle_state.rank0.pid790.json"
    source.parent.mkdir(parents=True)
    oracle.parent.mkdir(parents=True)
    source.write_text(
        json.dumps(
            {
                "traceEvents": [
                    {
                        "name": "hicache_insert_end",
                        "cat": "python_probe",
                        "ph": "X",
                        "ts": 10,
                        "dur": 1,
                        "pid": 790,
                        "tid": 1,
                        "args": {
                            "domain": "python_probe",
                            "model_input": True,
                            "event_role": "insert",
                            "tier_dst": "L1",
                            "page_identity": "p_extra",
                        },
                    }
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    oracle.write_text(
        json.dumps(
            {
                "traceEvents": [
                    {
                        "name": "hicache_insert_end:state_snapshot",
                        "cat": "python_probe",
                        "ph": "X",
                        "ts": 11,
                        "dur": 999,
                        "pid": 790,
                        "tid": 1,
                        "args": {
                            "domain": "python_probe",
                            "event_kind": "state_snapshot",
                            "model_input": False,
                            "state_snapshot": {
                                "enabled": True,
                                "derived": {
                                    "l1_resident_pages": [],
                                    "l2_resident_pages": [],
                                    "dirty_pages": [],
                                    "backuped_pages": [],
                                    "evicted_pages": [],
                                },
                            },
                        },
                    }
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    manifest = tmp / "state_mismatch/profile_manifest.json"
    manifest.write_text(
        json.dumps(
            {
                "run_dir": str(tmp / "state_mismatch"),
                "trace": {"torch_trace_files": [], "ld_preload_trace_files": []},
                "sidecar": {"python_probe_files": [{"path": str(source), "exists": True}]},
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    config = tmp / "state_mismatch/modeling_cache_state_mismatch.json"
    output_dir = tmp / "state_mismatch/modeling_out"
    config.write_text(
        json.dumps(
            {
                "input": {"profile_manifest": str(manifest)},
                "output_dir": str(output_dir),
                "mode": "cache_state",
                "validation": {
                    "hicache_state": {
                        "enabled": True,
                        "oracle_trace_paths": [str(oracle)],
                    },
                },
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
            "--emit-validation",
        ],
        cwd=ROOT,
    )
    validation = json.loads((output_dir / "validation.json").read_text(encoding="utf-8"))
    mismatch = validation["hicache_state"]["first_mismatch"]
    assert validation["validation_ready"] is False, validation
    assert "hicache_final_state_mismatch" in validation["validation_errors"], validation
    assert mismatch["tier"] == "l1_resident_pages", validation
    assert mismatch["page"] == "p_extra", validation
    assert mismatch["candidate_transition"]["transition_kind"] == "add_l1_resident", validation


if __name__ == "__main__":
    raise SystemExit(main())
