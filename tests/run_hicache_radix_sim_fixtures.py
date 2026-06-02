#!/usr/bin/env python3
from __future__ import annotations

import json
import tempfile
from pathlib import Path

import sys

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "src/modeling"))
sys.path.insert(0, str(REPO_ROOT / "scripts/trace"))

from trace_sim_model.hicache_radix_sim import RadixInputError, run_hicache_radix_sim
from inspect_hicache import summarize as inspect_hicache


def page_blocks(blocks: list[str], page_size: int = 64) -> list[str]:
    page_block_count = page_size // 32
    aligned = len(blocks) // page_block_count * page_block_count
    return [
        ",".join(blocks[i : i + page_block_count])
        for i in range(0, aligned, page_block_count)
    ]


def make_radix_op(
    ts: int,
    method: str,
    blocks: list[str],
    trace_page_size: int = 64,
    *,
    operation_id: str | int | None = None,
    node_id: str | int | None = None,
    parent_node_id: str | int | None = "",
    parent_blocks: list[str] | None = None,
    pid: int = 1,
) -> dict:
    raw_tokens = len(blocks) * 32
    aligned = raw_tokens // trace_page_size * trace_page_size
    block_hash = "|".join(blocks)
    trace_page_blocks = "|".join(page_blocks(blocks, trace_page_size))
    parent_block_hash = "|".join(parent_blocks or [])
    return {
        "name": "HiCache::radix_op",
        "cat": "hicache",
        "ph": "X",
        "ts": ts,
        "dur": 1,
        "pid": pid,
        "tid": pid,
        "args": {
            "framework": "sglang",
            "producer": "python_probe",
            "domain": "cache_io",
            "schema_version": "hicache_radix",
            "model_input": True,
            "event_kind": "radix_op",
            "method": method,
            "cache_id": "cache0",
            "op_seq": ts,
            "request_id": "req0",
            "operation_id": operation_id,
            "node_id": f"node_{ts}" if node_id is None else str(node_id),
            "parent_node_id": "" if parent_node_id is None else str(parent_node_id),
            "page_size": trace_page_size,
            "page_identity_kind": "block_tuple",
            "raw_token_len": raw_tokens,
            "aligned_token_len": aligned,
            "dropped_tail_tokens": raw_tokens - aligned,
            "num_tokens": raw_tokens,
            "block_size_tokens": 32,
            "block_keys_hash": block_hash,
            "node_local_block_keys_hash": "|".join(blocks[len(parent_blocks or []) :]),
            "full_path_block_keys_hash": block_hash,
            "parent_full_path_block_keys_hash": parent_block_hash,
            "trace_page_block_keys_hash": trace_page_blocks,
            "bytes_per_page": trace_page_size * 1024,
            "write_policy": "write_through",
            "status": "ok",
        },
    }


def make_storage_get(
    ts: int,
    blocks: list[str],
    page_size: int = 64,
    success_pages: int | None = None,
    *,
    operation_id: str | int = 0,
    runtime_page_keys: list[str] | None = None,
    pid: int = 1,
) -> dict:
    block_pages = page_blocks(blocks, page_size)
    page_keys = runtime_page_keys or [f"runtime_{i}" for i, _ in enumerate(block_pages)]
    if success_pages is None:
        success_pages = len(page_keys)
    return {
        "name": "HiCache::storage_op",
        "cat": "hicache",
        "ph": "X",
        "ts": ts,
        "dur": 1,
        "pid": pid,
        "tid": pid,
        "args": {
            "framework": "sglang",
            "producer": "python_probe",
            "domain": "cache_io",
            "schema_version": "hicache_radix",
            "model_input": True,
            "event_kind": "storage_op",
            "python_class": "HiCacheFile",
            "python_method": "batch_get",
            "method": "batch_get",
            "operation_id": str(operation_id),
            "request_id": "req0",
            "cache_id": "cache0",
            "tier_src": "L3",
            "tier_dst": "L2",
            "direction": "prefetch",
            "page_size": page_size,
            "page_identity_kind": "block_tuple",
            "num_tokens": success_pages * page_size,
            "num_pages": success_pages,
            "bytes_per_page": page_size * 1024,
            "success_pages": success_pages,
            "storage_success_pages": success_pages,
            "page_keys_hash": "|".join(page_keys[:success_pages]),
            "runtime_page_keys_hash": "|".join(page_keys[:success_pages]),
            "trace_page_block_keys_hash": "|".join(block_pages[:success_pages]),
            "status": "completed" if success_pages == len(page_keys) else "partial",
        },
    }


def make_storage_query(ts: int, blocks: list[str], page_size: int = 64, hit_pages: int = 0, *, operation_id: str | int = 0) -> dict:
    block_pages = page_blocks(blocks, page_size)
    page_keys = [f"runtime_{i}" for i, _ in enumerate(block_pages)]
    return {
        "name": "HiCache::storage_op",
        "cat": "hicache",
        "ph": "X",
        "ts": ts,
        "dur": 1,
        "pid": 1,
        "tid": 1,
        "args": {
            "framework": "sglang",
            "producer": "python_probe",
            "domain": "cache_io",
            "schema_version": "hicache_radix",
            "model_input": True,
            "event_kind": "storage_op",
            "python_class": "HiCacheFile",
            "python_method": "batch_exists",
            "method": "batch_exists",
            "operation_id": str(operation_id),
            "request_id": "req0",
            "cache_id": "cache0",
            "tier_src": "L3",
            "tier_dst": "L2",
            "direction": "query",
            "page_size": page_size,
            "page_identity_kind": "block_tuple",
            "num_pages": len(page_keys),
            "queried_pages": len(page_keys),
            "hit_pages": hit_pages,
            "success_pages": hit_pages,
            "miss_pages": len(page_keys) - hit_pages,
            "storage_hit_tokens": hit_pages * page_size,
            "page_keys_hash": "|".join(page_keys),
            "runtime_page_keys_hash": "|".join(page_keys),
            "trace_page_block_keys_hash": "|".join(block_pages),
            "status": "miss" if hit_pages == 0 else "hit",
        },
    }


def make_cache_operation(
    ts: int,
    operation_kind: str,
    stage: str,
    page_keys: list[str] | None = None,
    blocks: list[str] | None = None,
    *,
    operation_id: str | int = 0,
    node_id: str | int = "node_load",
) -> dict:
    page_keys = page_keys or []
    blocks = blocks or []
    return {
        "name": "HiCache::cache_operation",
        "cat": "hicache",
        "ph": "X",
        "ts": ts,
        "dur": 1,
        "pid": 1,
        "tid": 1,
        "args": {
            "framework": "sglang",
            "producer": "python_probe",
            "domain": "cache_io",
            "schema_version": "hicache_radix",
            "model_input": True,
            "event_kind": "cache_operation",
            "method": operation_kind,
            "operation_id": str(operation_id),
            "request_id": "req0",
            "operation_kind": operation_kind,
            "stage": stage,
            "node_id": str(node_id),
            "page_size": 64,
            "page_identity_kind": "block_tuple",
            "num_pages": len(page_keys),
            "page_keys_hash": "|".join(page_keys),
            "full_path_block_keys_hash": "|".join(blocks),
            "trace_page_block_keys_hash": "|".join(page_blocks(blocks, 64)),
            "status": "ok",
        },
    }


def run_trace(temp_dir: Path, events: list[dict], page_size: int, write_policy: str = "write_through", prefetch_policy: str = "trace_replay", l2_capacity: str | int = "infinite") -> dict:
    trace_path = temp_dir / f"trace_{page_size}_{write_policy}_{prefetch_policy}_{len(events)}.json"
    trace_path.write_text(json.dumps({"traceEvents": events}), encoding="utf-8")
    config = {
        "domains": ["cache_io"],
        "cache_io": {
            "page_size_tokens": page_size,
            "page_size_policy": "scenario",
            "block_size_tokens": 32,
            "bytes_per_page": page_size * 1024,
            "write_policy": write_policy,
            "prefetch_policy": prefetch_policy,
            "prefetch_threshold": 64,
            "tiers": [
                {"name": "L1", "capacity_pages": "infinite", "bandwidth_gbps": "inf"},
                {"name": "L2", "capacity_pages": l2_capacity, "bandwidth_gbps": "inf"},
                {"name": "L3", "capacity_pages": "infinite", "bandwidth_gbps": "inf"},
            ],
        },
    }
    result = run_hicache_radix_sim([trace_path], config, scenario_name=f"page{page_size}_{write_policy}_{prefetch_policy}")
    result["trace_path"] = trace_path
    return result


def run_case(temp_dir: Path, page_size: int, write_policy: str = "write_through") -> dict:
    prefix = [f"p{i}" for i in range(24)]  # 768 tokens.
    suffix = ["s0", "s1"]  # 64-token suffix.
    events = [
        make_radix_op(1, "insert", prefix),
        make_radix_op(2, "insert", prefix + suffix),
        make_radix_op(3, "evict", []),
    ]
    return run_trace(temp_dir, events, page_size, write_policy)["cache_io_summary"]


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        temp_dir = Path(tmp)
        page32 = run_case(temp_dir, 32)
        page64 = run_case(temp_dir, 64)
        page128 = run_case(temp_dir, 128)
        write_back = run_case(temp_dir, 64, "write_back")
        read_blocks = ["r0", "r1"]
        read_runtime_key = "runtime_read_page"
        read_success = run_trace(
            temp_dir,
            [
                make_radix_op(1, "insert", ["seed0", "seed1"]),
                make_cache_operation(2, "prefetch", "created", [read_runtime_key], read_blocks, operation_id=0),
                make_storage_query(2, read_blocks, hit_pages=1, operation_id=0),
                make_storage_get(3, read_blocks, runtime_page_keys=[read_runtime_key], operation_id=0),
                make_cache_operation(4, "prefetch", "completed", [read_runtime_key], read_blocks, operation_id=0),
                make_cache_operation(4, "load", "completed", [read_runtime_key], read_blocks, operation_id=0, node_id=0),
                make_radix_op(5, "load_back", read_blocks, operation_id=0, node_id=0, parent_node_id=0),
            ],
            64,
        )
        read_miss = run_trace(
            temp_dir,
            [
                make_radix_op(1, "insert", ["seed0", "seed1"]),
                make_cache_operation(2, "prefetch", "created", [read_runtime_key], read_blocks, operation_id=0),
                make_storage_query(2, read_blocks, hit_pages=0, operation_id=0),
                make_cache_operation(3, "load", "completed", [read_runtime_key], read_blocks, operation_id=0, node_id=0),
                make_radix_op(3, "load_back", read_blocks, operation_id=0, node_id=0, parent_node_id=0),
            ],
            64,
        )
        demand = run_trace(
            temp_dir,
            [
                make_radix_op(1, "insert", ["a0", "a1"]),
                make_radix_op(2, "insert", ["a0", "a1", "b0", "b1"]),
                make_cache_operation(3, "load", "completed", ["runtime_a", "runtime_b"], ["a0", "a1", "b0", "b1"], operation_id=0, node_id=0),
                make_radix_op(3, "load_back", ["a0", "a1", "b0", "b1"], operation_id=0, node_id=0, parent_node_id=0),
            ],
            64,
            prefetch_policy="none",
            l2_capacity=1,
        )
        two_rank_same_page = run_trace(
            temp_dir,
            [
                make_radix_op(1, "insert", ["rank_shared0", "rank_shared1"], pid=1),
                make_radix_op(1, "insert", ["rank_shared0", "rank_shared1"], pid=2),
            ],
            64,
        )
        missing_full_path_event = make_radix_op(1, "insert", ["missing0", "missing1"])
        missing_full_path_event["args"].pop("full_path_block_keys_hash")
        missing_full_path_event["args"].pop("block_keys_hash")
        missing_full_path_event["args"].pop("trace_page_block_keys_hash")
        try:
            run_trace(temp_dir, [missing_full_path_event], 64)
        except RadixInputError:
            pass
        else:
            raise AssertionError("radix_sim should reject radix_op without full_path_block_keys_hash")
        missing_load_link = [
            make_radix_op(1, "insert", ["seed0", "seed1"]),
            make_radix_op(2, "load_back", read_blocks, operation_id=None, node_id=0, parent_node_id=0),
        ]
        try:
            run_trace(temp_dir, missing_load_link, 64)
        except RadixInputError:
            pass
        else:
            raise AssertionError("radix_sim should reject load_back without a linked load operation")
        bad_parent_prefix = [
            make_radix_op(1, "insert", ["pa0", "pa1"], node_id="parent", parent_node_id=0),
            make_radix_op(2, "insert", ["pa0", "pa1", "child0", "child1"], node_id="child", parent_node_id="parent", parent_blocks=["wrong0", "wrong1"]),
        ]
        try:
            run_trace(temp_dir, bad_parent_prefix, 64)
        except RadixInputError:
            pass
        else:
            raise AssertionError("radix_sim should reject inconsistent parent full path")
        read_report = inspect_hicache([read_success["trace_path"]])
        miss_report = inspect_hicache([read_miss["trace_path"]])

    assert page32["pages_by_edge"]["L1->L2"] == 26
    assert page64["pages_by_edge"]["L1->L2"] == 13
    assert page128["pages_by_edge"]["L1->L2"] == 6
    assert page32["transfer_events"] == page64["transfer_events"] == 4
    assert page128["transfer_events"] == 2
    assert page128["pages_by_edge"]["L2->L3"] == 6
    assert write_back["pages_by_edge"]["L1->L2"] == 13
    assert write_back["pages_by_edge"]["L2->L3"] == 13
    assert read_success["cache_io_summary"]["pages_by_edge"]["L3->L2"] == 1
    assert read_success["cache_io_summary"]["pages_by_edge"]["L2->L1"] == 1
    assert "radix_sim_l2_l3_miss_on_load_back" not in read_success["cache_io_summary"]["whatif_warnings"]
    assert read_success["cache_io_summary"]["input_readiness"]["no_load_back_replay_miss"]
    assert read_success["cache_io_summary"]["storage_read_events_used"] == 1
    assert read_report["whatif_readiness"]["storage_query_ready"]
    assert read_report["whatif_readiness"]["storage_read_ready"]
    assert read_report["whatif_readiness"]["radix_sim_ready"]
    assert read_report["model_input_kind_counts"]["storage_op"] == 2
    assert not miss_report["storage_positive_counts"].get("query_hit")
    assert "no_storage_hit" in miss_report["whatif_readiness"]["missing"]
    assert demand["cache_io_summary"]["pages_by_edge"]["L3->L2"] >= 1
    assert demand["cache_io_summary"]["pages_by_edge"]["L2->L1"] >= 1
    assert len(two_rank_same_page["cache_io_summary"]["resident_pages_by_rank"]) == 2
    assert two_rank_same_page["cache_io_summary"]["resident_pages_by_tier"]["L1"] == 2
    print("hicache radix_sim fixtures passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
