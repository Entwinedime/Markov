#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from collections import Counter
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional


PAGE_IDENTITY_KIND = "block_tuple"
MODEL_INPUT_KINDS = {"radix_op", "storage_op", "cache_operation"}

HICACHE_KEYS = (
    "schema_version",
    "model_input",
    "framework",
    "producer",
    "domain",
    "event_kind",
    "request_id",
    "operation_id",
    "op_id",
    "node_id",
    "tier_src",
    "tier_dst",
    "direction",
    "pool",
    "pool_name",
    "transfer_scope",
    "num_tokens",
    "num_pages",
    "page_size",
    "page_identity_kind",
    "trace_page_block_keys_hash",
    "runtime_page_keys_hash",
    "bytes",
    "bytes_per_page",
    "page_keys_hash",
    "key_truncated",
    "layout",
    "io_backend",
    "storage_backend",
    "write_policy",
    "status",
    "method",
    "op_seq",
    "cache_id",
    "raw_token_len",
    "aligned_token_len",
    "dropped_tail_tokens",
    "parent_node_id",
    "node_key_len",
    "hit_count",
    "backuped",
    "evicted",
    "block_keys_hash",
    "node_local_block_keys_hash",
    "full_path_block_keys_hash",
    "parent_full_path_block_keys_hash",
    "block_key_truncated",
    "block_size_tokens",
    "radix_warning",
    "operation_kind",
    "stage",
    "queried_pages",
    "hit_pages",
    "miss_pages",
    "success_pages",
    "storage_success_pages",
    "storage_hit_tokens",
    "loaded_from_storage_tokens",
    "rejected_reason",
)

NUMERIC_KEYS = ("num_tokens", "num_pages", "page_size", "bytes", "bytes_per_page")
MOVEMENT_COVERAGE_KEYS = ("num_pages", "bytes", "page_keys_hash", "request_id", "operation_id")
RADIX_COVERAGE_KEYS = (
    "raw_token_len",
    "aligned_token_len",
    "block_keys_hash",
    "full_path_block_keys_hash",
    "node_local_block_keys_hash",
    "trace_page_block_keys_hash",
    "block_size_tokens",
    "method",
    "op_seq",
)


def load_events(path: Path) -> List[Dict[str, Any]]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return []
    if isinstance(data, dict):
        events = data.get("traceEvents", [])
    else:
        events = data
    return [event for event in events if isinstance(event, dict)]


def is_hicache_event(event: Dict[str, Any]) -> bool:
    args = event.get("args", {})
    if not isinstance(args, dict):
        args = {}
    name = str(event.get("name", ""))
    return event.get("cat") == "hicache" or name.startswith("HiCache::") or args.get("domain") == "cache_io"


def discover_trace_files(path: Path) -> List[Path]:
    if path.is_file():
        return [path]
    if not path.is_dir():
        return []

    merged_patterns = (
        "trace/merged/merged_trace.pid*.json",
        "**/trace/merged/merged_trace.pid*.json",
    )
    probe_patterns = (
        "trace/python_probe/python_probe_trace.rankunknown.pid*.json",
        "trace/python_probe/python_probe_trace.rank*.pid*.json",
        "**/trace/python_probe/python_probe_trace.rankunknown.pid*.json",
        "**/trace/python_probe/python_probe_trace.rank*.pid*.json",
    )

    for patterns in (probe_patterns, merged_patterns):
        files: List[Path] = []
        seen = set()
        for pattern in patterns:
            for candidate in sorted(path.glob(pattern)):
                if candidate.is_file() and candidate not in seen:
                    seen.add(candidate)
                    files.append(candidate)
        if files:
            return files

    patterns = ("*.json",)
    files: List[Path] = []
    seen = set()
    for pattern in patterns:
        for candidate in sorted(path.glob(pattern)):
            if candidate.is_file() and candidate not in seen:
                seen.add(candidate)
                files.append(candidate)
    return files


def as_positive_number(value: Any) -> Optional[float]:
    if value is None:
        return None
    try:
        numeric = float(value)
    except (TypeError, ValueError):
        return None
    return numeric if numeric > 0 else None


def as_text(value: Any) -> str:
    if value is None or value == "":
        return ""
    return str(value)


def split_pipe(value: Any) -> List[str]:
    if value is None or value == "":
        return []
    return [part for part in str(value).split("|") if part]


def split_block_pages(value: Any) -> List[List[str]]:
    pages = []
    for page in split_pipe(value):
        blocks = [part for part in page.split(",") if part]
        if blocks:
            pages.append(blocks)
    return pages


def int_or_none(value: Any) -> Optional[int]:
    if value is None or value == "":
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def edge_name(args: Dict[str, Any]) -> str:
    src = args.get("tier_src") or ""
    dst = args.get("tier_dst") or ""
    if not src and not dst:
        return "unknown"
    return f"{src}->{dst}" if dst else f"{src}->"


def event_kind(args: Dict[str, Any], edge: str) -> str:
    kind = args.get("event_kind")
    if kind not in (None, ""):
        return str(kind)
    if "->" in edge and edge != "unknown":
        return "movement"
    direction = args.get("direction")
    if direction in ("evict", "release", "insert"):
        return "movement"
    return "control"


def is_python_probe_event(args: Dict[str, Any]) -> bool:
    return args.get("producer") == "python_probe" and bool(args.get("python_method") or args.get("python_function"))


def is_physical_transfer_event(args: Dict[str, Any], edge: str, kind: str) -> bool:
    if "->" not in edge or edge.endswith("->") or edge == "unknown":
        return False
    if not is_python_probe_event(args):
        return kind == "movement"
    if args.get("python_function"):
        return True
    method = args.get("python_method")
    return method in {
        "start_writing",
        "start_loading",
        "_generic_page_set",
        "_page_set_zero_copy",
        "_generic_page_get",
        "_page_get_zero_copy",
        "load_back",
        "_draft_page_set",
        "_draft_page_get",
        "batch_get",
        "batch_get_v1",
        "batch_get_v2",
        "batch_set",
        "batch_set_v1",
        "batch_set_v2",
    }


def is_state_movement_event(args: Dict[str, Any], edge: str, kind: str) -> bool:
    direction = args.get("direction")
    if direction in ("insert", "evict", "release"):
        return True
    if not is_python_probe_event(args):
        return kind == "movement" and ("->" not in edge or edge.endswith("->") or edge == "unknown")
    method = args.get("python_method")
    return method in {
        "insert",
        "evict_device",
        "evict_host",
        "_evict_backuped",
        "_evict_regular",
        "append_host_mem_release",
        "_append_host_mem_release_pages",
    }


def summarize(paths: Iterable[Path], sample_limit: int = 5) -> Dict[str, Any]:
    files = []
    events_by_name: Counter[str] = Counter()
    events_by_pid: Counter[str] = Counter()
    status_counts: Counter[str] = Counter()
    direction_counts: Counter[str] = Counter()
    edge_counts: Counter[str] = Counter()
    edge_counts_by_kind: Counter[str] = Counter()
    event_kind_counts: Counter[str] = Counter()
    storage_backend_counts: Counter[str] = Counter()
    key_present: Counter[str] = Counter()
    movement_key_present: Counter[str] = Counter()
    numeric_nonzero: Counter[str] = Counter()
    movement_numeric_nonzero: Counter[str] = Counter()
    replay_edge_counts: Counter[str] = Counter()
    replay_movement_key_present: Counter[str] = Counter()
    replay_movement_numeric_nonzero: Counter[str] = Counter()
    radix_key_present: Counter[str] = Counter()
    radix_numeric_nonzero: Counter[str] = Counter()
    storage_event_counts: Counter[str] = Counter()
    storage_positive_counts: Counter[str] = Counter()
    model_input_events = 0
    debug_events = 0
    invalid_model_input_events = 0
    model_input_kind_counts: Counter[str] = Counter()
    model_input_key_present: Counter[str] = Counter()
    rejected_reasons: Counter[str] = Counter()
    radix_args: List[Dict[str, Any]] = []
    storage_args: List[Dict[str, Any]] = []
    cache_operation_args: List[Dict[str, Any]] = []
    model_pids: List[Any] = []
    sample_events: List[Dict[str, Any]] = []
    total_trace_events = 0
    hicache_events = 0
    transfer_events = 0
    min_ts: Optional[float] = None
    max_ts: Optional[float] = None

    for path in paths:
        events = load_events(path)
        total_trace_events += len(events)
        file_hicache = 0
        for event in events:
            if not is_hicache_event(event):
                continue

            args = event.get("args", {})
            if not isinstance(args, dict):
                args = {}

            file_hicache += 1
            hicache_events += 1
            name = str(event.get("name", "unknown"))
            events_by_name[name] += 1
            events_by_pid[str(event.get("pid", "unknown"))] += 1
            status_counts[str(args.get("status", "missing"))] += 1
            direction_counts[str(args.get("direction", "missing"))] += 1
            edge = edge_name(args)
            kind = event_kind(args, edge)
            is_model_input = args.get("model_input") is True
            if is_model_input:
                model_input_events += 1
                model_input_kind_counts[kind] += 1
                if args.get("domain") == "cache_io" and kind in MODEL_INPUT_KINDS:
                    model_pids.append(event.get("pid"))
                    if kind == "radix_op":
                        radix_args.append(args)
                    elif kind == "storage_op":
                        storage_args.append(args)
                    elif kind == "cache_operation":
                        cache_operation_args.append(args)
            else:
                debug_events += 1
            if (
                args.get("domain") == "cache_io"
                and kind in MODEL_INPUT_KINDS
                and not is_model_input
            ):
                invalid_model_input_events += 1
                rejected_reasons[str(args.get("rejected_reason", "missing:model_input"))] += 1
            replay_movement = is_physical_transfer_event(args, edge, kind) or is_state_movement_event(args, edge, kind)
            event_kind_counts[kind] += 1
            edge_counts[edge] += 1
            edge_counts_by_kind[f"{kind}:{edge}"] += 1
            if kind == "movement" and "->" in edge and not edge.endswith("->") and edge != "unknown":
                transfer_events += 1
            if replay_movement:
                replay_edge_counts[edge] += 1
            storage_backend_counts[str(args.get("storage_backend", "missing"))] += 1
            method = str(args.get("python_method", ""))
            if method in {"batch_exists", "batch_exists_v2", "_storage_hit_query"} or name in {"HiCache::storage_query", "HiCache::storage_op"} and str(args.get("direction")) == "query":
                storage_event_counts["query"] += 1
                if as_positive_number(args.get("hit_pages")) is not None or as_positive_number(args.get("success_pages")) is not None or as_positive_number(args.get("storage_hit_tokens")) is not None:
                    storage_positive_counts["query_hit"] += 1
            if method in {"batch_get", "batch_get_v1", "batch_get_v2"}:
                storage_event_counts["read"] += 1
                if as_positive_number(args.get("success_pages")) is not None or as_positive_number(args.get("storage_success_pages")) is not None:
                    storage_positive_counts["read_success"] += 1
            if method in {"batch_set", "batch_set_v1", "batch_set_v2"}:
                storage_event_counts["write"] += 1
                if as_positive_number(args.get("success_pages")) is not None or as_positive_number(args.get("storage_success_pages")) is not None:
                    storage_positive_counts["write_success"] += 1

            for key in HICACHE_KEYS:
                value = args.get(key)
                if value not in (None, ""):
                    key_present[key] += 1
                    if is_model_input:
                        model_input_key_present[key] += 1
                    if kind == "movement":
                        movement_key_present[key] += 1
                    if replay_movement:
                        replay_movement_key_present[key] += 1
                    if kind == "radix_op":
                        radix_key_present[key] += 1
            for key in NUMERIC_KEYS:
                if as_positive_number(args.get(key)) is not None:
                    numeric_nonzero[key] += 1
                    if kind == "movement":
                        movement_numeric_nonzero[key] += 1
                    if replay_movement:
                        replay_movement_numeric_nonzero[key] += 1
                    if kind == "radix_op":
                        radix_numeric_nonzero[key] += 1

            ts = as_positive_number(event.get("ts"))
            dur = as_positive_number(event.get("dur")) or 0.0
            if ts is not None:
                min_ts = ts if min_ts is None else min(min_ts, ts)
                max_ts = ts + dur if max_ts is None else max(max_ts, ts + dur)

            if len(sample_events) < sample_limit:
                sample_events.append(
                    {
                        "name": name,
                        "ts": event.get("ts"),
                        "dur": event.get("dur"),
                        "pid": event.get("pid"),
                        "tid": event.get("tid"),
                        "args": {key: args.get(key) for key in HICACHE_KEYS if key in args},
                    }
                )

        files.append({"path": str(path), "trace_events": len(events), "hicache_events": file_hicache})

    key_coverage = {
        key: {
            "present": key_present.get(key, 0),
            "missing": max(0, hicache_events - key_present.get(key, 0)),
        }
        for key in HICACHE_KEYS
    }
    numeric_coverage = {
        key: {
            "nonzero": numeric_nonzero.get(key, 0),
            "zero_or_missing": max(0, hicache_events - numeric_nonzero.get(key, 0)),
        }
        for key in NUMERIC_KEYS
    }
    movement_events = event_kind_counts.get("movement", 0)
    movement_key_coverage = {
        key: {
            "present": movement_key_present.get(key, 0),
            "missing": max(0, movement_events - movement_key_present.get(key, 0)),
        }
        for key in MOVEMENT_COVERAGE_KEYS
    }
    movement_numeric_coverage = {
        key: {
            "nonzero": movement_numeric_nonzero.get(key, 0),
            "zero_or_missing": max(0, movement_events - movement_numeric_nonzero.get(key, 0)),
        }
        for key in NUMERIC_KEYS
    }

    replay_movement_events = sum(replay_edge_counts.values())
    radix_events = event_kind_counts.get("radix_op", 0)
    has_movement = replay_movement_events > 0
    has_bytes_or_pages = replay_movement_numeric_nonzero.get("bytes", 0) > 0 or replay_movement_numeric_nonzero.get("num_pages", 0) > 0
    has_keys = replay_movement_key_present.get("page_keys_hash", 0) > 0
    has_radix_ops = radix_events > 0
    has_block_identity = radix_key_present.get("block_keys_hash", 0) > 0
    has_raw_lengths = radix_key_present.get("raw_token_len", 0) > 0 or radix_numeric_nonzero.get("num_tokens", 0) > 0
    has_prefetch = replay_edge_counts.get("L3->L2", 0) > 0
    has_load = replay_edge_counts.get("L2->L1", 0) > 0
    has_storage_query = storage_event_counts.get("query", 0) > 0
    has_storage_query_hit = storage_positive_counts.get("query_hit", 0) > 0
    has_storage_read = storage_positive_counts.get("read_success", 0) > 0
    has_inferable_prefetch = has_storage_read
    has_inferable_load = has_load
    observed_readback_ready = has_movement and has_keys and has_prefetch and has_load
    inferred_readback_ready = has_keys and has_inferable_prefetch and has_inferable_load
    radix_events = len(radix_args)
    storage_events = len(storage_args)
    cache_operations = len(cache_operation_args)

    path_radix_args = [
        args
        for args in radix_args
        if str(args.get("method") or args.get("python_method") or "") not in {"evict", "evict_host"}
    ]
    radix_full_path_ready = bool(path_radix_args) and all(args.get("full_path_block_keys_hash") for args in path_radix_args)
    page_identity_map_ready = bool(path_radix_args) and all(
        args.get("page_identity_kind") == PAGE_IDENTITY_KIND and args.get("trace_page_block_keys_hash")
        for args in path_radix_args
    )

    node_ids = {as_text(args.get("node_id")) for args in radix_args if as_text(args.get("node_id"))}
    parent_prefix_ready = True
    parent_node_exists_ready = True
    for args in path_radix_args:
        parent_id = as_text(args.get("parent_node_id"))
        parent_blocks = split_pipe(args.get("parent_full_path_block_keys_hash"))
        local_blocks = split_pipe(args.get("node_local_block_keys_hash"))
        full_blocks = split_pipe(args.get("full_path_block_keys_hash"))
        if parent_id and parent_id != "0" and parent_id not in node_ids:
            parent_node_exists_ready = False
        if parent_id and parent_id != "0" and not parent_blocks:
            parent_prefix_ready = False
        if parent_blocks and full_blocks[: len(parent_blocks)] != parent_blocks:
            parent_prefix_ready = False
        if local_blocks and (parent_blocks or parent_id) and parent_blocks + local_blocks != full_blocks:
            parent_prefix_ready = False
    parent_chain_ready = parent_prefix_ready and parent_node_exists_ready

    storage_identity_events = 0
    storage_read_operation_ids = set()
    storage_write_operation_ids = set()
    for args in storage_args:
        runtime_pages = split_pipe(args.get("page_keys_hash"))
        block_pages = split_block_pages(args.get("trace_page_block_keys_hash"))
        direction = str(args.get("direction") or "")
        if direction == "query":
            expected_pages = int_or_none(args.get("queried_pages"))
        else:
            expected_pages = int_or_none(args.get("success_pages"))
            if expected_pages is None:
                expected_pages = int_or_none(args.get("num_pages"))
        identity_ok = (
            args.get("page_identity_kind") == PAGE_IDENTITY_KIND
            and expected_pages is not None
            and len(runtime_pages) == len(block_pages) == expected_pages
        )
        if identity_ok:
            storage_identity_events += 1
        operation_id = as_text(args.get("operation_id"))
        if operation_id:
            if args.get("tier_src") == "L2" and args.get("tier_dst") == "L3":
                storage_write_operation_ids.add(operation_id)
            else:
                storage_read_operation_ids.add(operation_id)
    storage_op_ready = storage_events == storage_identity_events
    runtime_page_alias_ready = storage_events == 0 or storage_op_ready

    cache_operation_ids = {as_text(args.get("operation_id")) for args in cache_operation_args if as_text(args.get("operation_id"))}
    prefetch_operation_ids = {
        as_text(args.get("operation_id"))
        for args in cache_operation_args
        if as_text(args.get("operation_id")) and args.get("operation_kind") == "prefetch"
    }
    load_operation_ids = {
        as_text(args.get("operation_id"))
        for args in cache_operation_args
        if as_text(args.get("operation_id")) and args.get("operation_kind") == "load"
    }
    write_operation_ids = {
        as_text(args.get("operation_id"))
        for args in cache_operation_args
        if as_text(args.get("operation_id")) and args.get("operation_kind") == "write"
    }
    load_back_operation_ids = {
        as_text(args.get("operation_id"))
        for args in radix_args
        if as_text(args.get("operation_id")) and str(args.get("method") or args.get("python_method") or "") == "load_back"
    }
    load_back_count = sum(1 for args in radix_args if str(args.get("method") or args.get("python_method") or "") == "load_back")
    cache_operation_ready = cache_operations > 0
    storage_link_ready = (
        (storage_read_operation_ids.issubset(prefetch_operation_ids) if storage_read_operation_ids else True)
        and (storage_write_operation_ids.issubset(write_operation_ids) if storage_write_operation_ids else True)
    )
    load_back_link_ready = load_back_count == 0 or (
        len(load_back_operation_ids) == load_back_count and load_back_operation_ids.issubset(load_operation_ids)
    )
    operation_lifecycle_ready = storage_link_ready and load_back_link_ready
    operation_link_ready = operation_lifecycle_ready
    state_scope_ready = bool(model_pids) and all(pid not in (None, "") for pid in model_pids)
    no_load_back_replay_miss = None
    radix_sim_ready = (
        radix_full_path_ready
        and page_identity_map_ready
        and parent_chain_ready
        and storage_op_ready
        and runtime_page_alias_ready
        and state_scope_ready
        and operation_lifecycle_ready
        and invalid_model_input_events == 0
    )
    whatif_readiness = {
        "latency_bandwidth_ready": has_movement and has_bytes_or_pages,
        "capacity_eviction_ready": has_movement and has_keys,
        "observed_readback_ready": observed_readback_ready,
        "inferred_readback_ready": inferred_readback_ready,
        "prefetch_policy_ready": observed_readback_ready or inferred_readback_ready,
        "storage_query_ready": has_storage_query,
        "storage_read_ready": has_storage_read,
        "prefetch_complete_ready": has_prefetch or has_storage_read,
        "load_back_ready": has_load,
        "read_path_ready": (has_prefetch or has_storage_read) and has_load,
        "policy_simulation_ready": has_radix_ops and has_block_identity and has_raw_lengths,
        "page_identity_ready": has_keys,
        "radix_op_ready": has_radix_ops and has_block_identity and has_raw_lengths,
        "block_identity_ready": has_block_identity,
        "radix_full_path_ready": radix_full_path_ready,
        "page_identity_map_ready": page_identity_map_ready,
        "runtime_page_alias_ready": runtime_page_alias_ready,
        "state_scope_ready": state_scope_ready,
        "parent_chain_ready": parent_chain_ready,
        "parent_prefix_ready": parent_prefix_ready,
        "parent_node_exists_ready": parent_node_exists_ready,
        "operation_link_ready": operation_link_ready,
        "operation_lifecycle_ready": operation_lifecycle_ready,
        "load_back_link_ready": load_back_link_ready,
        "storage_op_ready": storage_op_ready,
        "cache_operation_ready": cache_operation_ready,
        "no_load_back_replay_miss": no_load_back_replay_miss,
        "radix_sim_ready": radix_sim_ready,
        "missing": [],
    }
    if not has_movement:
        whatif_readiness["missing"].append("movement_events")
    if not has_bytes_or_pages:
        whatif_readiness["missing"].append("movement_num_pages_or_bytes")
    if not has_keys:
        whatif_readiness["missing"].append("movement_page_keys_hash")
    if not has_radix_ops:
        whatif_readiness["missing"].append("radix_op_events")
    if not has_block_identity:
        whatif_readiness["missing"].append("radix_block_keys_hash")
    if radix_events and not radix_full_path_ready:
        whatif_readiness["missing"].append("radix_full_path_block_keys_hash")
    if radix_events and not page_identity_map_ready:
        whatif_readiness["missing"].append("trace_page_block_keys_hash")
    if storage_events and not runtime_page_alias_ready:
        whatif_readiness["missing"].append("runtime_page_alias")
    if radix_events and not parent_prefix_ready:
        whatif_readiness["missing"].append("parent_prefix")
    if radix_events and not parent_node_exists_ready:
        whatif_readiness["missing"].append("parent_node")
    if not operation_link_ready:
        whatif_readiness["missing"].append("operation_link")
    if not state_scope_ready:
        whatif_readiness["missing"].append("pid_scope")
    if invalid_model_input_events:
        whatif_readiness["missing"].append("invalid_model_input_events")
    if not has_raw_lengths:
        whatif_readiness["missing"].append("radix_raw_token_len")
    if not has_prefetch:
        whatif_readiness["missing"].append("movement_L3_to_L2_prefetch")
    if not has_load:
        whatif_readiness["missing"].append("movement_L2_to_L1_load")
    if has_storage_query and not has_storage_query_hit:
        whatif_readiness["missing"].append("no_storage_hit")
    if not has_storage_query:
        whatif_readiness["missing"].append("storage_query_events")
    if not has_inferable_prefetch and not has_prefetch:
        whatif_readiness["missing"].append("inferable_L3_to_L2_prefetch")
    if not has_inferable_load and not has_load:
        whatif_readiness["missing"].append("inferable_L2_to_L1_load")

    return {
        "files": files,
        "total_files": len(files),
        "total_trace_events": total_trace_events,
        "hicache_events": hicache_events,
        "transfer_events": transfer_events,
        "time_range_us": None if min_ts is None or max_ts is None else [min_ts, max_ts],
        "events_by_name": dict(events_by_name),
        "events_by_pid": dict(events_by_pid),
        "event_kind_counts": dict(event_kind_counts),
        "status_counts": dict(status_counts),
        "direction_counts": dict(direction_counts),
        "edge_counts": dict(edge_counts),
        "edge_counts_by_kind": dict(edge_counts_by_kind),
        "replay_edge_counts": dict(replay_edge_counts),
        "storage_event_counts": dict(storage_event_counts),
        "storage_positive_counts": dict(storage_positive_counts),
        "model_input_events": model_input_events,
        "debug_events": debug_events,
        "invalid_model_input_events": invalid_model_input_events,
        "model_input_kind_counts": dict(model_input_kind_counts),
        "rejected_reasons": dict(rejected_reasons),
        "operation_ids": {
            "cache_operation_ids": sorted(cache_operation_ids),
            "prefetch_operation_ids": sorted(prefetch_operation_ids),
            "write_operation_ids": sorted(write_operation_ids),
            "load_operation_ids": sorted(load_operation_ids),
            "storage_read_operation_ids": sorted(storage_read_operation_ids),
            "storage_write_operation_ids": sorted(storage_write_operation_ids),
            "load_back_operation_ids": sorted(load_back_operation_ids),
        },
        "storage_backend_counts": dict(storage_backend_counts),
        "key_coverage": key_coverage,
        "numeric_coverage": numeric_coverage,
        "movement_key_coverage": movement_key_coverage,
        "movement_numeric_coverage": movement_numeric_coverage,
        "radix_op_events": radix_events,
        "radix_key_coverage": {
            key: {
                "present": radix_key_present.get(key, 0),
                "missing": max(0, radix_events - radix_key_present.get(key, 0)),
            }
            for key in RADIX_COVERAGE_KEYS
        },
        "whatif_readiness": whatif_readiness,
        "sample_events": sample_events,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Inspect HiCache/cache_io events from trace files or a profile run directory.")
    parser.add_argument("path", help="Trace JSON file, experiment dir, or profile run dir")
    parser.add_argument("--output", "-o", help="Write summary JSON to this path")
    parser.add_argument("--samples", type=int, default=5, help="Number of sample events to include")
    args = parser.parse_args()

    paths = discover_trace_files(Path(args.path))
    report = summarize(paths, sample_limit=max(0, args.samples))
    payload = json.dumps(report, indent=2, sort_keys=True)
    if args.output:
        output = Path(args.output)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(payload + "\n", encoding="utf-8")
    else:
        print(payload)
    return 0 if report["hicache_events"] > 0 else 2


if __name__ == "__main__":
    raise SystemExit(main())
