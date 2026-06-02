#!/usr/bin/env python3
from __future__ import annotations

import json
import os
import subprocess
import tempfile
from copy import deepcopy
from pathlib import Path
from typing import Any, Dict


REPO_ROOT = Path(__file__).resolve().parents[1]
TRACE_GRAPH_BIN = Path(os.environ.get("TRACE_GRAPH_BIN", REPO_ROOT / "build/bin/trace_graph"))
FIXTURE_DIR = REPO_ROOT / "tests/fixtures/cache_io"
MODEL_CONFIG: Dict[str, Any] = {
    "domains": ["cache_io"],
    "cache_io": {
        "page_size_tokens": 16,
        "bytes_per_page": 1024,
        "tiers": [
            {"name": "L1", "capacity_pages": 8, "latency_us": 0, "bandwidth_gbps": "inf", "eviction": "lru"},
            {"name": "L2", "capacity_pages": 16, "latency_us": 10, "bandwidth_gbps": 20, "eviction": "lru"},
            {"name": "L3", "capacity_pages": "infinite", "latency_us": 100, "bandwidth_gbps": 5, "eviction": "infinite"},
        ],
        "write_policy": "trace",
        "prefetch_policy": "trace_replay",
    },
}


def run_fixture(name: str, temp_dir: Path, config: Dict[str, Any] | None = None) -> Dict[str, Any]:
    config_path = temp_dir / f"{name}.model_config.json"
    config_path.write_text(json.dumps(config or MODEL_CONFIG), encoding="utf-8")
    summary = temp_dir / f"{name}.summary.json"
    output = temp_dir / f"{name}.graph.json"
    subprocess.run(
        [
            str(TRACE_GRAPH_BIN),
            "--model-config",
            str(config_path),
            "--model-summary",
            str(summary),
            "--output",
            str(output),
            "--no-raw",
            str(FIXTURE_DIR / f"{name}.json"),
        ],
        check=True,
        cwd=REPO_ROOT,
    )
    return json.loads(summary.read_text(encoding="utf-8"))


def assert_equal(actual: Any, expected: Any, message: str) -> None:
    if actual != expected:
        raise AssertionError(f"{message}: expected {expected!r}, got {actual!r}")


def assert_greater(actual: Any, expected: Any, message: str) -> None:
    if not actual > expected:
        raise AssertionError(f"{message}: expected {actual!r} > {expected!r}")


def assert_less(actual: Any, expected: Any, message: str) -> None:
    if not actual < expected:
        raise AssertionError(f"{message}: expected {actual!r} < {expected!r}")


def config_with(**cache_io_updates: Any) -> Dict[str, Any]:
    config = deepcopy(MODEL_CONFIG)
    for key, value in cache_io_updates.items():
        config["cache_io"][key] = value
    return config


def config_with_tier(tier_name: str, **tier_updates: Any) -> Dict[str, Any]:
    config = deepcopy(MODEL_CONFIG)
    for tier in config["cache_io"]["tiers"]:
        if tier["name"] == tier_name:
            tier.update(tier_updates)
    return config


def main() -> int:
    if not TRACE_GRAPH_BIN.exists():
        raise SystemExit(f"trace_graph binary not found: {TRACE_GRAPH_BIN}")

    with tempfile.TemporaryDirectory(prefix="trace_graph_fixtures_") as tmp:
        temp_dir = Path(tmp)

        simple = run_fixture("simple_l3_l2_l1", temp_dir)
        assert_equal(simple["events"], 3, "simple events")
        assert_equal(simple["transfer_events"], 2, "simple transfer events")
        assert_equal(simple["bytes_by_edge"].get("L3->L2"), 2048, "simple L3->L2 bytes")
        assert_equal(simple["bytes_by_edge"].get("L2->L1"), 2048, "simple L2->L1 bytes")
        assert_equal(simple["events_with_bytes"], 2, "simple byte coverage")
        assert_equal(simple["missing_bytes_events"], 0, "simple missing bytes count")

        writeback = run_fixture("writeback_evict", temp_dir)
        assert_equal(writeback["events"], 3, "writeback events")
        assert_equal(writeback["transfer_events"], 2, "writeback transfer events")
        assert_equal(writeback["writeback_events"], 2, "writeback count")
        assert_equal(writeback["eviction_events"], 1, "eviction count")
        assert_equal(writeback["bytes_by_edge"].get("L1->L2"), 3072, "writeback L1->L2 bytes")
        assert_equal(writeback["bytes_by_edge"].get("L2->L3"), 2048, "writeback L2->L3 bytes")

        missing = run_fixture("missing_hicache", temp_dir)
        assert_equal(missing["events"], 0, "missing events")
        assert_equal(missing["transfer_events"], 0, "missing transfer events")

        movement_control = run_fixture("movement_control", temp_dir)
        assert_equal(movement_control["events"], 2, "movement/control events")
        assert_equal(movement_control["movement_events_used"], 1, "movement/control movement count")
        assert_equal(movement_control["control_events_ignored"], 1, "movement/control ignored count")
        assert_equal(movement_control["transfer_events"], 1, "movement/control transfer count")

        storage_base = run_fixture("storage_bandwidth", temp_dir)
        storage_fast = run_fixture("storage_bandwidth", temp_dir, config_with_tier("L3", bandwidth_gbps=10))
        storage_slow = run_fixture("storage_bandwidth", temp_dir, config_with_tier("L3", bandwidth_gbps=2.5))
        assert_less(
            storage_fast["estimated_latency_us"],
            storage_base["estimated_latency_us"],
            "L3 bandwidth x2 should reduce estimated latency",
        )
        assert_greater(
            storage_slow["estimated_latency_us"],
            storage_base["estimated_latency_us"],
            "L3 bandwidth /2 should increase estimated latency",
        )

        page32 = run_fixture("page_size_override", temp_dir, config_with(page_size_tokens=32, page_size_policy="scenario"))
        page128 = run_fixture("page_size_override", temp_dir, config_with(page_size_tokens=128, page_size_policy="scenario"))
        assert_greater(
            page32["pages_by_edge"].get("L3->L2", 0),
            page128["pages_by_edge"].get("L3->L2", 0),
            "scenario page size should change replay page count",
        )
        assert_greater(
            page32["bytes_by_edge"].get("L3->L2", 0),
            page128["bytes_by_edge"].get("L3->L2", 0),
            "scenario page size with fixed bytes_per_page should change replay bytes",
        )

        keyed_lru = run_fixture("keyed_l2_replay", temp_dir, config_with_tier("L2", capacity_pages=2, eviction="lru"))
        keyed_fifo = run_fixture("keyed_l2_replay", temp_dir, config_with_tier("L2", capacity_pages=2, eviction="fifo"))
        keyed_small = run_fixture("keyed_l2_replay", temp_dir, config_with_tier("L2", capacity_pages=1, eviction="lru"))
        assert_equal(keyed_lru["miss_pages_by_tier"].get("L2", 0), 0, "L2 LRU capacity 2 should keep page-a resident")
        assert_greater(keyed_fifo["miss_pages_by_tier"].get("L2", 0), 0, "L2 FIFO should evict page-a in keyed fixture")
        assert_greater(
            keyed_small["evictions_by_tier"].get("L2", 0),
            keyed_lru["evictions_by_tier"].get("L2", 0),
            "smaller L2 capacity should increase evictions",
        )

        prefetch_none = run_fixture("keyed_l2_replay", temp_dir, config_with(prefetch_policy="none"))
        assert_greater(
            prefetch_none["estimated_latency_us"],
            keyed_lru["estimated_latency_us"],
            "disabling prefetch should move storage cost to demand load",
        )

        inferred = run_fixture("query_queue_ack_inference", temp_dir, config_with(prefetch_policy="wait_complete"))
        assert_equal(inferred["observed_movements_used"], 0, "inference fixture should not use observed movements")
        assert_equal(inferred["inferred_movements_used"], 0, "query/queue events should not be replayed without a physical movement point")
        assert_equal(inferred["transfer_events"], 0, "query/queue diagnostic events are not physical transfer count")

        write_through = run_fixture(
            "capacity_lru_writeback",
            temp_dir,
            config_with_tier("L2", capacity_pages=2, eviction="lru"),
        )
        write_back_config = config_with_tier("L2", capacity_pages=2, eviction="lru")
        write_back_config["cache_io"]["write_policy"] = "write_back"
        write_back = run_fixture("capacity_lru_writeback", temp_dir, write_back_config)
        assert_equal(write_through["writebacks_by_edge"].get("L2->L3", 0), 0, "write-through fixture has no model writeback")
        assert_greater(
            write_back["writebacks_by_edge"].get("L2->L3", 0),
            write_through["writebacks_by_edge"].get("L2->L3", 0),
            "write-back should write dirty victims on L2 eviction",
        )
        assert_greater(
            write_back["model_generated_movements"],
            write_through["model_generated_movements"],
            "write-back eviction should create model-generated movements",
        )

    print("trace_graph cache_io fixtures passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
