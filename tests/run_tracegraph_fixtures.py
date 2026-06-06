#!/usr/bin/env python3
"""C++ TraceGraph fixtures。"""

from __future__ import annotations

import json
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    with tempfile.TemporaryDirectory() as raw_tmp:
        tmp = Path(raw_tmp)
        run_summary = run_trace(tmp, ROOT / "tests/fixtures/modeling/tracegraph_basic.json", "basic")
        assert run_summary["node_count"] == 3, run_summary
        assert run_summary["edge_count"] >= 2, run_summary
        assert run_summary["simulated_e2e_ns"] > 0, run_summary

        same_stream = write_trace(
            tmp / "same_stream.json",
            [
                event("k1", "Kernel", 0, 10, {"streamId": "0", "device_id": 0}),
                event("k2", "Kernel", 20, 20, {"streamId": "0", "device_id": 0}),
            ],
        )
        same_stream_summary = run_trace(tmp, same_stream, "same_stream")
        assert same_stream_summary["edge_count"] == 1, same_stream_summary
        assert same_stream_summary["simulated_e2e_ns"] == 30, same_stream_summary
        assert same_stream_summary["real_e2e_ns"] == 40, same_stream_summary
        assert "stage_timings_ms" in same_stream_summary, same_stream_summary

        parallel_stream = write_trace(
            tmp / "parallel_stream.json",
            [
                event("k1", "Kernel", 0, 10, {"streamId": "0", "device_id": 0}, tid=1),
                event("k2", "Kernel", 0, 20, {"streamId": "1", "device_id": 0}, tid=2),
            ],
        )
        parallel_summary = run_trace(tmp, parallel_stream, "parallel_stream")
        assert parallel_summary["edge_count"] == 0, parallel_summary
        assert parallel_summary["simulated_e2e_ns"] == 20, parallel_summary

        correlation = write_trace(
            tmp / "correlation.json",
            [
                event("runtime_launch", "runtime", 0, 5, {"correlation_id": "c1"}),
                event("kernel", "Kernel", 10, 10, {"streamId": "0", "device_id": 0, "correlation_id": "c1"}),
            ],
        )
        correlation_summary = run_trace(tmp, correlation, "correlation")
        assert correlation_summary["edge_count"] >= 1, correlation_summary
        assert correlation_summary["simulated_e2e_ns"] == 15, correlation_summary

        sync = write_trace(
            tmp / "sync.json",
            [
                event("kernel", "Kernel", 0, 40, {"streamId": "0", "device_id": 0}),
                event("AscendCL@aclrtSynchronizeStream", "runtime", 10, 5, {"Raw Stream": "0"}),
            ],
        )
        sync_summary = run_trace(tmp, sync, "sync")
        assert sync_summary["simulated_e2e_ns"] == 50, sync_summary

        nested = write_trace(
            tmp / "nested.json",
            [
                event("outer", "runtime", 0, 100, {}),
                event("inner", "runtime", 10, 10, {}),
                event("after", "runtime", 120, 5, {}),
            ],
        )
        nested_summary = run_trace(tmp, nested, "nested")
        assert nested_summary["parsed_record_count"] == 3, nested_summary
        assert nested_summary["node_count"] == 2, nested_summary

        connection_skip = write_trace(
            tmp / "connection_skip.json",
            [
                event("a", "runtime", 0, 1, {"connection_id": "x"}),
                event("b", "runtime", 2, 1, {"connection_id": "x"}),
                event("c", "runtime", 4, 1, {"connection_id": "x"}),
            ],
        )
        connection_skip_summary = run_trace(tmp, connection_skip, "connection_skip")
        assert connection_skip_summary["edge_counts_by_kind"].get("correlation", 0) == 0, connection_skip_summary

        event_wait_same_lane = write_trace(
            tmp / "event_wait_same_lane.json",
            [
                event("AscendCL@aclrtRecordEvent", "runtime", 0, 1, {"connection_id": "record", "Event Id": "e1", "Raw Stream": "raw"}),
                event("EVENT_RECORD", "runtime", 2, 1, {"connection_id": "record", "Physic Stream Id": "7"}, tid=7),
                event("AscendCL@aclrtStreamWaitEvent", "runtime", 3, 1, {"connection_id": "wait", "Event Id": "e1"}),
                event("EVENT_WAIT", "runtime", 4, 1, {"connection_id": "wait", "Physic Stream Id": "7"}, tid=7),
            ],
        )
        event_wait_same_lane_summary = run_trace(tmp, event_wait_same_lane, "event_wait_same_lane")
        assert event_wait_same_lane_summary["edge_counts_by_kind"].get("sync", 0) == 0, event_wait_same_lane_summary

        raw_stream_sync = write_trace(
            tmp / "raw_stream_sync.json",
            [
                event("AscendCL@aclrtRecordEvent", "runtime", 0, 1, {"connection_id": "record", "Event Id": "e1", "Raw Stream": "raw"}),
                event("EVENT_RECORD", "runtime", 2, 1, {"connection_id": "record", "Physic Stream Id": "1816"}, tid=7),
                event("AscendCL@aclrtSynchronizeStream", "runtime", 4, 1, {"Raw Stream": "raw"}),
            ],
        )
        raw_stream_sync_summary = run_trace(tmp, raw_stream_sync, "raw_stream_sync")
        assert raw_stream_sync_summary["edge_counts_by_kind"].get("sync", 0) == 1, raw_stream_sync_summary

        missing_event_id = write_trace(
            tmp / "missing_event_id.json",
            [
                event("AscendCL@aclrtRecordEvent", "runtime", 0, 1, {"connection_id": "record"}),
                event("EVENT_RECORD", "runtime", 2, 1, {"connection_id": "record", "Physic Stream Id": "7"}, tid=7),
                event("AscendCL@aclrtStreamWaitEvent", "runtime", 3, 1, {"connection_id": "wait"}),
                event("EVENT_WAIT", "runtime", 4, 1, {"connection_id": "wait", "Physic Stream Id": "8"}, tid=8),
            ],
        )
        missing_event_id_summary = run_trace(tmp, missing_event_id, "missing_event_id")
        assert missing_event_id_summary["edge_counts_by_kind"].get("sync", 0) == 0, missing_event_id_summary

        stream_alias_sync = write_trace(
            tmp / "stream_alias_sync.json",
            [
                event("kernel", "Kernel", 0, 40, {"streamId": "s7", "device_id": 0}, tid=7),
                event("AscendCL@aclrtSynchronizeStream", "runtime", 10, 5, {"streamId": "s7"}),
            ],
        )
        stream_alias_sync_summary = run_trace(tmp, stream_alias_sync, "stream_alias_sync")
        assert stream_alias_sync_summary["edge_counts_by_kind"].get("sync", 0) == 1, stream_alias_sync_summary
        assert stream_alias_sync_summary["simulated_e2e_ns"] == 50, stream_alias_sync_summary

        stream_timeout_sync = write_trace(
            tmp / "stream_timeout_sync.json",
            [
                event("kernel", "Kernel", 0, 40, {"streamId": "s7", "device_id": 0}, tid=7),
                event("AscendCL@aclrtSynchronizeStreamWithTimeout", "runtime", 10, 5, {"streamId": "s7"}),
            ],
        )
        stream_timeout_sync_summary = run_trace(tmp, stream_timeout_sync, "stream_timeout_sync")
        assert stream_timeout_sync_summary["edge_counts_by_kind"].get("sync", 0) == 1, stream_timeout_sync_summary
        assert stream_timeout_sync_summary["simulated_e2e_ns"] == 50, stream_timeout_sync_summary

        event_sync = write_trace(
            tmp / "event_sync.json",
            [
                event("AscendCL@aclrtRecordEvent", "runtime", 0, 1, {"connection_id": "record", "Event Id": "e2"}),
                event("EVENT_RECORD", "runtime", 5, 1, {"connection_id": "record", "Physic Stream Id": "7"}, tid=7),
                event("AscendCL@aclrtSynchronizeEvent", "runtime", 6, 5, {"Event Id": "e2"}),
            ],
        )
        event_sync_summary = run_trace(tmp, event_sync, "event_sync")
        assert event_sync_summary["edge_counts_by_kind"].get("sync", 0) == 1, event_sync_summary

        device_sync = write_trace(
            tmp / "device_sync.json",
            [
                event("k1", "Kernel", 0, 40, {"streamId": "s7", "device_id": 0}, tid=7),
                event("k2", "Kernel", 0, 60, {"streamId": "s8", "device_id": 0}, tid=8),
                event("AscendCL@aclrtSynchronizeDevice", "runtime", 10, 5, {}),
            ],
        )
        device_sync_summary = run_trace(tmp, device_sync, "device_sync")
        assert device_sync_summary["edge_counts_by_kind"].get("sync", 0) == 2, device_sync_summary
        assert device_sync_summary["simulated_e2e_ns"] == 70, device_sync_summary

        nested_args_stream_select = write_trace(
            tmp / "nested_args_stream_select.json",
            [
                event("k1", "Kernel", 0, 40, {"streamId": "s7", "device_id": 0}, tid=7),
                event("k2", "Kernel", 0, 80, {"streamId": "s8", "device_id": 0}, tid=8),
                event("AscendCL@aclrtSynchronizeStream", "runtime", 10, 5, {"Function-Args": {"ignored": {"value": 1}}, "streamId": "s7"}),
            ],
        )
        nested_args_stream_select_summary = run_trace(tmp, nested_args_stream_select, "nested_args_stream_select")
        assert nested_args_stream_select_summary["edge_counts_by_kind"].get("sync", 0) == 1, nested_args_stream_select_summary

        cpu_gap = write_trace(
            tmp / "cpu_gap.json",
            [
                event("cpu_a", "runtime", 0, 10, {}),
                event("cpu_b", "runtime", 20, 5, {}),
            ],
        )
        cpu_gap_summary = run_trace(tmp, cpu_gap, "cpu_gap")
        assert cpu_gap_summary["simulated_e2e_ns"] == 25, cpu_gap_summary
    print("tracegraph fixtures passed")
    return 0


def event(name: str, cat: str, ts: int, dur: int, args: dict[str, object], tid: int = 1) -> dict[str, object]:
    return {"name": name, "cat": cat, "ph": "X", "ts": ts, "dur": dur, "pid": 1, "tid": tid, "args": args}


def write_trace(path: Path, events: list[dict[str, object]]) -> Path:
    path.write_text(json.dumps({"traceEvents": events}, ensure_ascii=False), encoding="utf-8")
    return path


def run_trace(tmp: Path, trace_path: Path, name: str) -> dict[str, object]:
    output = tmp / f"{name}_graph.json"
    summary = tmp / f"{name}_summary.json"
    subprocess.check_call(
        [
            str(ROOT / "build/bin/trace_graph"),
            "--input",
            str(trace_path),
            "--graph-output",
            str(output),
            "--full-output",
            "--run-summary",
            str(summary),
            "--scenario-name",
            name,
        ],
        cwd=ROOT,
    )
    graph = json.loads(output.read_text(encoding="utf-8"))
    assert isinstance(graph.get("traceEvents"), list), graph
    return json.loads(summary.read_text(encoding="utf-8"))


if __name__ == "__main__":
    raise SystemExit(main())
