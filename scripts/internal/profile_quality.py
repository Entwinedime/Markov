#!/usr/bin/env python3
"""Profiling 质量审计。

本脚本只检查采集事实是否齐备，不做建模判断。它的主要用途是跑完真实
profiling 后快速判断 Python probe target 是否命中，以及哪些字段缺失会阻塞
后续 HiCache modeling。
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


ROOT_DIR = Path(__file__).resolve().parents[2]
CONTAINER_REPO_PREFIXES = ("/workspace/trace-sim", "/opt/trace-sim")


@dataclass
class TargetQuality:
    """单个 Python probe target 的命中和字段质量。"""

    target_id: str
    configured: bool = True
    target: str = ""
    events_total: int = 0
    phases: Counter[str] = field(default_factory=Counter)
    statuses: Counter[str] = field(default_factory=Counter)
    missing_required_fields: Counter[str] = field(default_factory=Counter)
    field_presence: Counter[str] = field(default_factory=Counter)
    request_ids: set[str] = field(default_factory=set)
    operation_ids: set[str] = field(default_factory=set)
    node_ids: set[str] = field(default_factory=set)

    def observe(self, args: dict[str, Any]) -> None:
        self.events_total += 1
        self.phases[str(args.get("phase") or "unknown")] += 1
        self.statuses[str(args.get("status") or "unknown")] += 1
        for field in args.get("missing_required_fields") or []:
            self.missing_required_fields[str(field)] += 1
        for field, value in args.items():
            if value is not None and field not in {"missing_required_fields"}:
                self.field_presence[field] += 1
        _add_optional(self.request_ids, args.get("request_id"))
        _add_optional(self.operation_ids, args.get("operation_id"))
        _add_optional(self.node_ids, args.get("node_id"))
        _add_optional(self.node_ids, args.get("last_host_node_id"))
        _add_optional(self.node_ids, args.get("last_device_node_id"))
        _add_optional(self.node_ids, args.get("best_match_node_id"))

    def to_dict(self) -> dict[str, Any]:
        return {
            "configured": self.configured,
            "target": self.target,
            "events_total": self.events_total,
            "phases": dict(sorted(self.phases.items())),
            "statuses": dict(sorted(self.statuses.items())),
            "missing_required_fields": dict(sorted(self.missing_required_fields.items())),
            "field_presence": dict(sorted(self.field_presence.items())),
            "request_id_count": len(self.request_ids),
            "operation_id_count": len(self.operation_ids),
            "node_id_count": len(self.node_ids),
        }


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    manifest_path = resolve_path(args.manifest)
    result = audit_profile(manifest_path)
    output_path = resolve_output_path(args.output, manifest_path, result)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(output_path)
    return 0 if result.get("quality_ready") else 1


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Audit profiling trace quality.")
    parser.add_argument("--manifest", required=True, help="profile_manifest.json path")
    parser.add_argument("--output", help="output JSON path; defaults to run_dir/profile_quality.json")
    return parser.parse_args(argv)


def audit_profile(manifest_path: Path) -> dict[str, Any]:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    run_dir = map_repo_path(Path(str(manifest.get("run_dir") or manifest_path.parent)))
    profiling = manifest.get("profiling") if isinstance(manifest.get("profiling"), dict) else {}
    trace = manifest.get("trace") if isinstance(manifest.get("trace"), dict) else {}
    sidecar = manifest.get("sidecar") if isinstance(manifest.get("sidecar"), dict) else {}

    configured_targets = _configured_targets(profiling)
    channels_enabled = {
        str(channel)
        for channel in profiling.get("channels_enabled") or []
        if isinstance(channel, str)
    }
    hicache_state_trace_enabled = bool(profiling.get("python_state_trace_enabled", False))
    python_channel_enabled = "python" in channels_enabled or bool(configured_targets)
    target_quality = {
        target_id: TargetQuality(
            target_id=target_id,
            configured=True,
            target=str(target.get("target") or ""),
        )
        for target_id, target in configured_targets.items()
    }

    python_probe_files = _existing_paths(sidecar.get("python_probe_files", []))
    events = _load_python_probe_events(python_probe_files)
    unknown_targets: dict[str, TargetQuality] = {}
    configured_mechanisms = _configured_mechanisms(configured_targets)
    mechanism_counts = Counter()
    invariant_accumulator = _new_hicache_invariant_accumulator()
    capacity_accumulator = _new_hicache_capacity_accumulator()
    for event in events:
        raw_args = event.get("args")
        if not isinstance(raw_args, dict):
            continue
        _observe_hicache_capacity(capacity_accumulator, raw_args)
        if _false_like(raw_args.get("model_input")):
            continue
        _observe_mechanism(mechanism_counts, raw_args)
        _observe_hicache_invariant(invariant_accumulator, raw_args)
        target_id = str(raw_args.get("target_id") or "unknown")
        quality = target_quality.get(target_id)
        if quality is None:
            quality = unknown_targets.setdefault(
                target_id,
                TargetQuality(target_id=target_id, configured=False),
            )
        quality.observe(raw_args)

    missing_targets = sorted(
        target_id
        for target_id, quality in target_quality.items()
        if quality.events_total == 0
    )
    targets_with_missing_fields = sorted(
        target_id
        for target_id, quality in target_quality.items()
        if quality.missing_required_fields
    )
    exception_targets = sorted(
        target_id
        for target_id, quality in {**target_quality, **unknown_targets}.items()
        if quality.phases.get("exception", 0) > 0 or quality.statuses.get("exception", 0) > 0
    )

    errors: list[str] = []
    if python_channel_enabled and not python_probe_files:
        errors.append("missing_python_probe_files")
    if configured_targets and len(missing_targets) == len(configured_targets):
        errors.append("all_python_probe_targets_missing")
    if targets_with_missing_fields:
        errors.append("python_probe_required_fields_missing")
    if exception_targets:
        errors.append("python_probe_exception_events")
    workload_report = _discover_workload_report(run_dir)
    expected_mechanisms = _expected_mechanisms_from_workload(workload_report)
    expected_configured_mechanisms = sorted(set(expected_mechanisms) & set(configured_mechanisms))
    missing_mechanisms = sorted(
        mechanism
        for mechanism in expected_configured_mechanisms
        if mechanism_counts.get(mechanism, 0) <= 0
    )
    if missing_mechanisms:
        errors.append("expected_hicache_mechanisms_missing")
    invariant_coverage = _finalize_hicache_invariant(invariant_accumulator)
    if invariant_coverage["missing_required_fact_events"] > 0:
        errors.append("hicache_invariant_facts_missing")
    if invariant_coverage["route_error_events"] > 0:
        errors.append("hicache_invariant_route_invalid")
    if invariant_coverage["missing_token_dictionary_refs"] or invariant_coverage["dictionary_ids_without_tokens"]:
        errors.append("hicache_token_dictionary_missing")
    if invariant_coverage["seq_order_error_count"] > 0:
        errors.append("hicache_invariant_seq_invalid")
    if hicache_state_trace_enabled and capacity_accumulator["snapshot_count"] <= 0:
        errors.append("hicache_capacity_snapshot_missing")

    torch_files = _existing_paths(trace.get("torch_trace_files", []))
    if not torch_files:
        torch_files = _existing_dir_files(trace.get("torch_trace_dir"), "**/trace_view.json")
    return {
        "manifest_path": str(manifest_path),
        "run_dir": str(run_dir),
        "profiling_ready": bool(manifest.get("profiling_ready")),
        "status": manifest.get("status"),
        "dry_run": bool(manifest.get("dry_run")),
        "trace_files": {
            "torch": len(torch_files),
            "ld_preload": len(_existing_paths(trace.get("ld_preload_trace_files", []))),
            "python_probe": len(python_probe_files),
        },
        "python_probe_events": len(events),
        "configured_target_count": len(configured_targets),
        "observed_target_count": sum(1 for quality in target_quality.values() if quality.events_total > 0),
        "missing_targets": missing_targets,
        "targets_with_missing_required_fields": targets_with_missing_fields,
        "exception_targets": exception_targets,
        "hicache_invariant_coverage": invariant_coverage,
        "workload_report": str(workload_report) if workload_report else None,
        "expected_cache_mechanisms": expected_mechanisms,
        "configured_cache_mechanisms": configured_mechanisms,
        "expected_configured_cache_mechanisms": expected_configured_mechanisms,
        "observed_cache_mechanisms": dict(sorted(mechanism_counts.items())),
        "missing_cache_mechanisms": missing_mechanisms,
        "hicache_state_trace_enabled": hicache_state_trace_enabled,
        "hicache_capacity_observed": capacity_accumulator["snapshot_count"] > 0,
        "hicache_capacity": _finalize_hicache_capacity(capacity_accumulator),
        "unknown_targets": sorted(unknown_targets),
        "targets": {
            target_id: quality.to_dict()
            for target_id, quality in sorted(target_quality.items())
        },
        "unknown_target_details": {
            target_id: quality.to_dict()
            for target_id, quality in sorted(unknown_targets.items())
        },
        "quality_errors": errors,
        "quality_ready": not errors,
    }


def resolve_path(value: str) -> Path:
    path = Path(value).expanduser()
    if path.is_absolute():
        return path
    return ROOT_DIR / path


def resolve_output_path(value: str | None, manifest_path: Path, result: dict[str, Any]) -> Path:
    if value:
        return resolve_path(value)
    run_dir = Path(str(result.get("run_dir") or manifest_path.parent))
    return run_dir / "profile_quality.json"


def map_repo_path(path: Path) -> Path:
    """把容器内 repo 路径映射到当前宿主工作区。"""

    raw = str(path)
    for prefix in CONTAINER_REPO_PREFIXES:
        if raw == prefix:
            return ROOT_DIR
        if raw.startswith(prefix + "/"):
            return ROOT_DIR / raw[len(prefix) + 1 :]
    return path


def _configured_targets(profiling: dict[str, Any]) -> dict[str, dict[str, Any]]:
    targets: dict[str, dict[str, Any]] = {}
    for item in profiling.get("python_targets") or []:
        if not isinstance(item, dict):
            continue
        target_id = item.get("id")
        if isinstance(target_id, str) and target_id:
            targets[target_id] = item
    return targets


def _existing_paths(entries: Any) -> list[Path]:
    paths: list[Path] = []
    if not isinstance(entries, list):
        return paths
    for item in entries:
        if not isinstance(item, dict) or not item.get("exists", True):
            continue
        path_value = item.get("path")
        if isinstance(path_value, str):
            path = map_repo_path(Path(path_value))
            if path.is_file():
                paths.append(path)
    return paths


def _existing_dir_files(raw_dir: Any, pattern: str) -> list[Path]:
    """从 manifest 目录字段递归发现已有文件。"""

    if not isinstance(raw_dir, str):
        return []
    directory = map_repo_path(Path(raw_dir))
    if not directory.is_dir():
        return []
    return sorted(item for item in directory.glob(pattern) if item.is_file())


def _discover_workload_report(run_dir: Path) -> Path | None:
    """查找 workload 输出，不把路径硬编码进 manifest。"""

    if not run_dir.is_dir():
        return None
    candidates = sorted(run_dir.glob("bench/**/workload_report.json"))
    return candidates[-1] if candidates else None


def _expected_mechanisms_from_workload(path: Path | None) -> list[str]:
    if path is None or not path.is_file():
        return []
    try:
        report = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return []
    raw = report.get("expected_cache_mechanisms")
    mechanisms: set[str] = set()
    if isinstance(raw, dict):
        for value in raw.values():
            if isinstance(value, list):
                mechanisms.update(str(item) for item in value)
    elif isinstance(raw, list):
        mechanisms.update(str(item) for item in raw)
    return sorted(mechanisms)


def _load_python_probe_events(paths: list[Path]) -> list[dict[str, Any]]:
    events: list[dict[str, Any]] = []
    for path in paths:
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            continue
        raw_events = data.get("traceEvents") if isinstance(data, dict) else None
        if not isinstance(raw_events, list):
            continue
        for event in raw_events:
            if isinstance(event, dict) and event.get("cat") == "python_probe":
                events.append(event)
    return events


_ROLE_TO_MECHANISM = {
    "request_tokens": "lookup",
    "lookup_path": "lookup",
    "insert_path": "insert",
    "prefetch_intent": "prefetch_schedule",
    "prefetch_check_point": "prefetch_progress",
    "capacity_request": "evict",
    "lock_scope_delta": "lock_ref",
    "prefetch_io_observed": "prefetch_transfer",
    "writeback_io_observed": "write_storage",
    "writeback_enqueue_observed": "write_storage",
}


_INVARIANT_REQUIRED_FIELDS_BY_ROLE = {
    "request_tokens": (
        "request_id",
        "cache_scope",
        "seq_no",
        "token_dictionary",
        "full_path_span",
        "token_count",
    ),
    "lookup_path": (
        "request_id",
        "cache_scope",
        "seq_no",
        "token_dictionary",
        "full_path_span",
    ),
    "insert_path": (
        "cache_scope",
        "seq_no",
        "token_dictionary",
        "full_path_span",
        "value_token_count",
        "prefix_len",
    ),
    "prefetch_intent": (
        "request_id",
        "cache_scope",
        "seq_no",
        "prefix_token_dictionary",
        "suffix_token_dictionary",
        "prefix_span",
        "suffix_span",
        "policy_params",
    ),
    "prefetch_check_point": (
        "request_id",
        "cache_scope",
        "seq_no",
        "check_kind",
    ),
    "capacity_request": (
        "cache_scope",
        "seq_no",
        "requested_tokens",
        "requested_pages_source",
        "reason",
        "tier",
        "policy_params",
    ),
    "lock_scope_delta": (
        "cache_scope",
        "seq_no",
        "node_token_dictionary",
        "logical_path_span",
        "delta",
        "lock_direction",
    ),
    "cache_config_observed": (
        "cache_scope",
        "seq_no",
        "source_page_size",
        "write_policy",
        "prefetch_policy",
        "thresholds",
        "capacity_summary",
    ),
}

_INVARIANT_EITHER_FIELDS_BY_ROLE = {
    "lookup_path": (("matched_span", "matched_token_len"),),
}

_INVARIANT_DICTIONARY_FIELDS_BY_ROLE = {
    "request_tokens": ("token_dictionary",),
    "lookup_path": ("token_dictionary", "matched_token_dictionary"),
    "insert_path": ("token_dictionary", "inserted_token_dictionary"),
    "prefetch_intent": (
        "prefix_token_dictionary",
        "suffix_token_dictionary",
        "full_token_dictionary",
    ),
    "lock_scope_delta": ("node_token_dictionary",),
}

_INVARIANT_SPAN_FIELDS_BY_ROLE = {
    "request_tokens": ("full_path_span",),
    "lookup_path": ("full_path_span", "matched_span"),
    "insert_path": ("full_path_span", "inserted_span"),
    "prefetch_intent": ("prefix_span", "suffix_span", "full_path_span"),
    "lock_scope_delta": ("logical_path_span",),
}


def _observe_mechanism(counter: Counter[str], args: dict[str, Any]) -> None:
    if args.get("phase") != "end":
        return
    event_role = str(args.get("event_role") or "")
    mechanism = _ROLE_TO_MECHANISM.get(event_role)
    if mechanism:
        counter[mechanism] += 1


def _configured_mechanisms(configured_targets: dict[str, dict[str, Any]]) -> list[str]:
    mechanisms: set[str] = set()
    for target in configured_targets.values():
        role = _configured_const_field(target, "event_role")
        mechanism = _ROLE_TO_MECHANISM.get(role)
        if mechanism:
            mechanisms.add(mechanism)
    return sorted(mechanisms)


def _configured_const_field(target: dict[str, Any], name: str) -> str:
    fields = target.get("fields") if isinstance(target.get("fields"), list) else []
    for field in fields:
        if not isinstance(field, dict) or field.get("name") != name:
            continue
        source = str(field.get("source") or "")
        if source.startswith("const:"):
            return source.split(":", 1)[1]
    return ""


def _new_hicache_invariant_accumulator() -> dict[str, Any]:
    return {
        "counts": Counter(),
        "role_end_events": Counter(),
        "missing_fields": Counter(),
        "missing_fields_by_role": defaultdict(Counter),
        "dictionary_ids": set(),
        "dictionary_ids_with_tokens": set(),
        "span_path_ids": set(),
        "seq_by_scope": defaultdict(list),
    }


def _observe_hicache_invariant(accumulator: dict[str, Any], args: dict[str, Any]) -> None:
    if not _is_hicache_profile_event(args):
        return
    _observe_token_references(accumulator, args)
    fact_class = str(args.get("fact_class") or "")
    state_model_input = not _false_like(args.get("state_model_input"))
    dag_input = not _false_like(args.get("dag_input"))
    if state_model_input and fact_class != "invariant_state":
        accumulator["counts"]["state_model_non_invariant_events"] += 1
    if fact_class != "invariant_state":
        return

    accumulator["counts"]["invariant_events"] += 1
    route_error = False
    if not state_model_input:
        accumulator["counts"]["invariant_without_state_model_input"] += 1
        route_error = True
    if dag_input:
        accumulator["counts"]["invariant_with_dag_input"] += 1
        route_error = True
    if route_error:
        accumulator["counts"]["route_error_events"] += 1

    role = str(args.get("event_role") or "")
    if role not in _INVARIANT_REQUIRED_FIELDS_BY_ROLE:
        accumulator["counts"]["unknown_invariant_role_events"] += 1
        if args.get("phase") == "end":
            accumulator["counts"]["missing_required_fact_events"] += 1
            accumulator["missing_fields"]["event_role"] += 1
        return
    if args.get("phase") != "end":
        return

    accumulator["counts"]["required_events"] += 1
    accumulator["role_end_events"][role] += 1
    missing = _missing_invariant_fields(args, role)
    if missing:
        accumulator["counts"]["missing_required_fact_events"] += 1
        for field in missing:
            accumulator["missing_fields"][field] += 1
            accumulator["missing_fields_by_role"][role][field] += 1

    scope = args.get("cache_scope")
    seq_no = _int_or_none(args.get("seq_no"))
    if _has_fact(scope) and seq_no is not None:
        accumulator["seq_by_scope"][str(scope)].append(seq_no)


def _observe_token_references(accumulator: dict[str, Any], args: dict[str, Any]) -> None:
    for value in args.values():
        if not isinstance(value, dict):
            continue
        token_path_id = value.get("token_path_id")
        if isinstance(token_path_id, str) and token_path_id:
            accumulator["dictionary_ids"].add(token_path_id)
            if isinstance(value.get("token_ids"), list) and value.get("token_ids"):
                accumulator["dictionary_ids_with_tokens"].add(token_path_id)
        path_id = value.get("path_id")
        if isinstance(path_id, str) and path_id:
            accumulator["span_path_ids"].add(path_id)


def _missing_invariant_fields(args: dict[str, Any], role: str) -> list[str]:
    missing = [
        field
        for field in _INVARIANT_REQUIRED_FIELDS_BY_ROLE.get(role, ())
        if not _has_fact(args.get(field))
    ]
    for choices in _INVARIANT_EITHER_FIELDS_BY_ROLE.get(role, ()):
        if not any(_has_fact(args.get(field)) for field in choices):
            missing.append("|".join(choices))

    for field in _INVARIANT_DICTIONARY_FIELDS_BY_ROLE.get(role, ()):
        value = args.get(field)
        if value is not None and not _has_token_dictionary(value):
            missing.append(f"{field}.token_path_id")
    for field in _INVARIANT_SPAN_FIELDS_BY_ROLE.get(role, ()):
        value = args.get(field)
        if value is not None and not _has_token_span(value):
            missing.append(f"{field}.path_id")
    return missing


def _finalize_hicache_invariant(accumulator: dict[str, Any]) -> dict[str, Any]:
    counts: Counter[str] = accumulator["counts"]
    missing_token_dictionary_refs = sorted(accumulator["span_path_ids"] - accumulator["dictionary_ids"])
    dictionary_ids_without_tokens = sorted(accumulator["dictionary_ids"] - accumulator["dictionary_ids_with_tokens"])
    seq_order_error_count = 0
    for seq_values in accumulator["seq_by_scope"].values():
        previous = None
        for value in seq_values:
            if previous is not None and value <= previous:
                seq_order_error_count += 1
            previous = value
    return {
        "invariant_events": counts["invariant_events"],
        "required_events": counts["required_events"],
        "role_end_events": dict(sorted(accumulator["role_end_events"].items())),
        "missing_required_fact_events": counts["missing_required_fact_events"],
        "missing_fields": dict(sorted(accumulator["missing_fields"].items())),
        "missing_fields_by_role": {
            role: dict(sorted(counter.items()))
            for role, counter in sorted(accumulator["missing_fields_by_role"].items())
        },
        "route_error_events": counts["route_error_events"]
        + counts["state_model_non_invariant_events"]
        + counts["unknown_invariant_role_events"],
        "state_model_non_invariant_events": counts["state_model_non_invariant_events"],
        "invariant_without_state_model_input": counts["invariant_without_state_model_input"],
        "invariant_with_dag_input": counts["invariant_with_dag_input"],
        "unknown_invariant_role_events": counts["unknown_invariant_role_events"],
        "token_dictionary_paths": len(accumulator["dictionary_ids"]),
        "token_dictionary_paths_with_token_ids": len(accumulator["dictionary_ids_with_tokens"]),
        "token_span_refs": len(accumulator["span_path_ids"]),
        "missing_token_dictionary_refs": missing_token_dictionary_refs,
        "dictionary_ids_without_tokens": dictionary_ids_without_tokens,
        "seq_scope_count": len(accumulator["seq_by_scope"]),
        "seq_order_error_count": seq_order_error_count,
        "ready": counts["missing_required_fact_events"] == 0
        and counts["route_error_events"] == 0
        and counts["state_model_non_invariant_events"] == 0
        and counts["unknown_invariant_role_events"] == 0
        and not missing_token_dictionary_refs
        and not dictionary_ids_without_tokens
        and seq_order_error_count == 0,
    }


def _has_fact(value: Any) -> bool:
    if value is None:
        return False
    if isinstance(value, str):
        return bool(value)
    if isinstance(value, (list, tuple, set, dict)):
        return len(value) > 0
    return True


def _has_token_dictionary(value: Any) -> bool:
    if not isinstance(value, dict):
        return False
    return (
        isinstance(value.get("token_path_id"), str)
        and bool(value.get("token_path_id"))
        and _has_fact(value.get("token_count"))
        and _has_fact(value.get("hash_algo"))
    )


def _has_token_span(value: Any) -> bool:
    if not isinstance(value, dict):
        return False
    return (
        isinstance(value.get("path_id"), str)
        and bool(value.get("path_id"))
        and _has_fact(value.get("begin"))
        and _has_fact(value.get("end"))
        and _has_fact(value.get("token_count"))
        and _has_fact(value.get("hash_algo"))
    )


def _int_or_none(value: Any) -> int | None:
    if value is None or isinstance(value, bool):
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def _is_hicache_profile_event(args: dict[str, Any]) -> bool:
    target_id = str(args.get("target_id") or "").lower()
    event_role = str(args.get("event_role") or "")
    if target_id.startswith(("hiradix.", "hicache.", "hicache_controller.")):
        return True
    return event_role in _ROLE_TO_MECHANISM or event_role in _INVARIANT_REQUIRED_FIELDS_BY_ROLE


def _false_like(value: Any) -> bool:
    return str(value).lower() in {"false", "0", "no", "off"}


def _new_hicache_capacity_accumulator() -> dict[str, Any]:
    return {
        "snapshot_count": 0,
        "object_type_counts": Counter(),
        "unique_values": defaultdict(set),
        "samples": [],
    }


def _observe_hicache_capacity(accumulator: dict[str, Any], args: dict[str, Any]) -> None:
    """从 validation-only state snapshot 中汇总 capacity/policy 证据。"""

    if str(args.get("event_kind") or "") != "state_snapshot":
        return
    snapshot = args.get("state_snapshot")
    if not isinstance(snapshot, dict) or not snapshot.get("enabled", False):
        return
    capacity = snapshot.get("capacity")
    if not isinstance(capacity, dict):
        return
    accumulator["snapshot_count"] += 1
    object_type = str(snapshot.get("object_type") or "unknown")
    accumulator["object_type_counts"][object_type] += 1
    for key, value in _flatten_capacity_scalars(capacity):
        accumulator["unique_values"][key].add(json.dumps(value, ensure_ascii=False, sort_keys=True))
    if len(accumulator["samples"]) < 5:
        accumulator["samples"].append(
            {
                "object_type": object_type,
                "page_size": capacity.get("page_size"),
                "write_policy": capacity.get("write_policy"),
                "prefetch_policy": capacity.get("prefetch_policy"),
                "l1_capacity_pages": capacity.get("l1_capacity_pages"),
                "l1_available_pages": capacity.get("l1_available_pages"),
                "l2_capacity_pages": capacity.get("l2_capacity_pages"),
                "l2_available_pages": capacity.get("l2_available_pages"),
                "prefetch_capacity_limit_pages": capacity.get("prefetch_capacity_limit_pages"),
            }
        )


def _finalize_hicache_capacity(accumulator: dict[str, Any]) -> dict[str, Any]:
    unique_values = {}
    for key, values in sorted(accumulator["unique_values"].items()):
        unique_values[key] = [json.loads(value) for value in sorted(values)]
    return {
        "ready": accumulator["snapshot_count"] > 0,
        "snapshot_count": accumulator["snapshot_count"],
        "object_type_counts": dict(sorted(accumulator["object_type_counts"].items())),
        "unique_values": unique_values,
        "samples": accumulator["samples"],
    }


def _flatten_capacity_scalars(value: Any, prefix: str = "") -> list[tuple[str, Any]]:
    rows: list[tuple[str, Any]] = []
    if isinstance(value, dict):
        for key, item in sorted(value.items()):
            child_prefix = f"{prefix}.{key}" if prefix else str(key)
            rows.extend(_flatten_capacity_scalars(item, child_prefix))
        return rows
    if value is None or isinstance(value, (list, tuple, set)):
        return rows
    if isinstance(value, (str, int, float, bool)):
        rows.append((prefix, value))
    return rows


def _add_optional(values: set[str], value: Any) -> None:
    if value is not None:
        values.add(str(value))


if __name__ == "__main__":
    raise SystemExit(main())
