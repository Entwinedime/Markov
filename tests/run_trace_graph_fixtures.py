#!/usr/bin/env python3
from __future__ import annotations

import json
import os
import subprocess
import tempfile
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


def run_fixture(name: str, temp_dir: Path) -> Dict[str, Any]:
    summary = temp_dir / f"{name}.summary.json"
    output = temp_dir / f"{name}.graph.json"
    subprocess.run(
        [
            str(TRACE_GRAPH_BIN),
            "--model-config",
            str(temp_dir / "model_config.json"),
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


def main() -> int:
    if not TRACE_GRAPH_BIN.exists():
        raise SystemExit(f"trace_graph binary not found: {TRACE_GRAPH_BIN}")

    with tempfile.TemporaryDirectory(prefix="trace_graph_fixtures_") as tmp:
        temp_dir = Path(tmp)
        (temp_dir / "model_config.json").write_text(json.dumps(MODEL_CONFIG), encoding="utf-8")

        simple = run_fixture("simple_l3_l2_l1", temp_dir)
        assert_equal(simple["events"], 3, "simple events")
        assert_equal(simple["transfer_events"], 2, "simple transfer events")
        assert_equal(simple["bytes_by_edge"].get("L3->L2"), 2048, "simple L3->L2 bytes")
        assert_equal(simple["bytes_by_edge"].get("L2->L1"), 2048, "simple L2->L1 bytes")
        assert_equal(simple["events_with_bytes"], 3, "simple byte coverage")
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

    print("trace_graph cache_io fixtures passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
