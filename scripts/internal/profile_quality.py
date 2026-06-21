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
SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from trace_json import load_chrome_trace_events  # noqa: E402


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
        """吸收一个 Python probe 事件参数，累计 target 质量指标。"""

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
        """输出稳定 JSON 摘要，避免把 set/Counter 原样泄露到报告。"""

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
    """CLI 入口：生成 profile_quality.json，并用退出码表达是否 ready。"""

    args = parse_args(argv)
    manifest_path = resolve_path(args.manifest)
    result = audit_profile(manifest_path)
    output_path = resolve_output_path(args.output, manifest_path, result)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(output_path)
    return 0 if result.get("quality_ready") else 1


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """解析 profiling quality 审计参数。"""

    parser = argparse.ArgumentParser(description="Audit profiling trace quality.")
    parser.add_argument("--manifest", required=True, help="profile_manifest.json path")
    parser.add_argument("--output", help="output JSON path; defaults to run_dir/profile_quality.json")
    return parser.parse_args(argv)


def audit_profile(manifest_path: Path) -> dict[str, Any]:
    """审计一次 profiling run 的采集质量。

    本函数只检查 trace/channel/target/fact 是否齐备，返回阻塞后续 modeling 的
    缺口列表；它不尝试从实际 trace 反推 HiCache 策略或 state transition。
    """

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
    """把 CLI 路径解析到当前 repo 视角。"""

    path = Path(value).expanduser()
    if path.is_absolute():
        return path
    return ROOT_DIR / path


def resolve_output_path(value: str | None, manifest_path: Path, result: dict[str, Any]) -> Path:
    """确定质量报告输出路径，默认写到 run_dir。"""

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
    """从 manifest profiling fragment 读取已配置 Python probe target。"""

    targets: dict[str, dict[str, Any]] = {}
    raw_targets = profiling.get("python_targets")
    if raw_targets is None:
        python_probe = profiling.get("python_probe") if isinstance(profiling.get("python_probe"), dict) else {}
        raw_targets = python_probe.get("targets")
    for item in raw_targets or []:
        if not isinstance(item, dict):
            continue
        target_id = item.get("id")
        if isinstance(target_id, str) and target_id:
            targets[target_id] = item
    return targets


def _existing_paths(entries: Any) -> list[Path]:
    """把 manifest sidecar path 条目过滤成当前宿主机可读文件。"""

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
    """从 workload report 读取期望命中的 HiCache 机制集合。"""

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
    """加载所有 Python probe Chrome trace event。"""

    events: list[dict[str, Any]] = []
    for path in paths:
        raw_events, _status = load_chrome_trace_events(path, auto_repair=True)
        for event in raw_events:
            if _is_python_probe_trace_event(event):
                events.append(event)
    return events


def _is_python_probe_trace_event(event: dict[str, Any]) -> bool:
    """判断 event 是否来自 Python probe。"""

    args = event.get("args") if isinstance(event.get("args"), dict) else {}
    return (
        event.get("cat") == "python_probe"
        or str(args.get("domain") or "") == "python_probe"
        or str(event.get("name") or "").startswith("hicache_")
    )


_ROLE_TO_MECHANISM = {
    "request_bound_match_anchor": "lookup",
    "cache_stage_match_path_observed": "lookup",
    "lookup_result_observed": "lookup",
    "request_lifecycle_anchor": "insert",
    "request_lifecycle_path_observed": "insert",
    "request_lifecycle_runtime_observed": "insert",
    "request_admission": "admission",
    "request_admission_observed": "admission",
    "insert_path": "insert",
    "insert_result_observed": "insert",
    "prefetch_decision": "prefetch_schedule",
    "prefetch_decision_observed": "prefetch_schedule",
    "prefetch_intent": "prefetch_schedule",
    "prefetch_intent_observed": "prefetch_schedule",
    "prefetch_check_point": "prefetch_progress",
    "prefetch_progress_observed": "prefetch_progress",
    "maintenance_checkpoint": "maintenance",
    "capacity_request": "evict",
    "capacity_result_observed": "evict",
    "lock_scope_delta": "lock_ref",
    "lock_scope_result_observed": "lock_ref",
    "host_ref_delta_observed": "lock_ref",
    "all_blocks_cleared_observed": "maintenance",
    "load_back_request_observed": "load_back",
    "load_back_result_observed": "load_back",
    "load_enqueue_observed": "load_back",
    "load_start_observed": "load_back",
    "writeback_schedule_observed": "write_storage",
    "writeback_storage_schedule_observed": "write_storage",
    "write_enqueue_observed": "write_storage",
    "write_start_observed": "write_storage",
    "host_eviction_observed": "evict",
    "prefetch_terminate_requested_observed": "prefetch_progress",
    "request_abort_cleanup_observed": "prefetch_progress",
    "prefetch_loaded_tokens_observed": "prefetch_progress",
    "prefetch_rate_limit_observed": "prefetch_schedule",
    "prefetch_enqueue_observed": "prefetch_schedule",
    "storage_hit_query_observed": "prefetch_schedule",
    "prefetch_terminate_observed": "prefetch_progress",
    "host_mem_release_enqueue_observed": "prefetch_progress",
    "node_store_observed": "insert",
    "node_remove_observed": "evict",
    "radix_node_mutation_observed": "insert",
    "evictable_state_observed": "evict",
    "write_counter_delta_observed": "write_storage",
    "write_ack_checkpoint_observed": "write_storage",
    "load_ack_checkpoint_observed": "load_back",
    "storage_control_checkpoint_observed": "maintenance",
    "prefetch_io_observed": "prefetch_transfer",
    "writeback_io_observed": "write_storage",
    "writeback_enqueue_observed": "write_storage",
}


_INVARIANT_REQUIRED_FIELDS_BY_ROLE = {
    "request_bound_match_anchor": (
        "request_id",
        "cache_scope",
        "seq_no",
        "source_page_size",
        "token_dictionary",
        "full_path_span",
        "token_count",
    ),
    "request_lifecycle_anchor": (
        "request_id",
        "cache_scope",
        "seq_no",
        "lifecycle_kind",
        "source_page_size",
    ),
    "request_admission": (
        "request_id",
        "cache_scope",
        "seq_no",
        "admission_kind",
        "source_page_size",
        "token_dictionary",
        "full_path_span",
        "token_count",
        "policy_params",
    ),
    "prefetch_decision": (
        "request_id",
        "cache_scope",
        "seq_no",
        "token_dictionary",
        "full_path_span",
        "token_count",
        "policy_params",
    ),
    "prefetch_check_point": (
        "request_id",
        "cache_scope",
        "seq_no",
        "check_kind",
    ),
}

_INVARIANT_EITHER_FIELDS_BY_ROLE = {
}

_INVARIANT_DICTIONARY_FIELDS_BY_ROLE = {
    "request_bound_match_anchor": ("token_dictionary",),
    "request_admission": ("token_dictionary",),
    "prefetch_decision": ("token_dictionary",),
}

_INVARIANT_SPAN_FIELDS_BY_ROLE = {
    "request_bound_match_anchor": ("full_path_span",),
    "request_admission": ("full_path_span",),
    "prefetch_decision": ("full_path_span",),
}


def _observe_mechanism(counter: Counter[str], args: dict[str, Any]) -> None:
    """把 end-phase 事件角色映射为 workload 机制命中。"""

    if args.get("phase") != "end":
        return
    event_role = str(args.get("event_role") or "")
    mechanism = _ROLE_TO_MECHANISM.get(event_role)
    if mechanism:
        counter[mechanism] += 1


def _configured_mechanisms(configured_targets: dict[str, dict[str, Any]]) -> list[str]:
    """根据配置 target 的 fact.role 推导理论可观测机制。"""

    mechanisms: set[str] = set()
    for target in configured_targets.values():
        role = _configured_fact_role(target)
        mechanism = _ROLE_TO_MECHANISM.get(role)
        if mechanism:
            mechanisms.add(mechanism)
    return sorted(mechanisms)


def _configured_fact_role(target: dict[str, Any]) -> str:
    """读取 target 配置中的 fact.role。"""

    fact = target.get("fact")
    if isinstance(fact, dict):
        role = fact.get("role")
        if isinstance(role, str):
            return role
    return ""


def _new_hicache_invariant_accumulator() -> dict[str, Any]:
    """创建 HiCache invariant fact 覆盖率累加器。"""

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
    """检查单个事件是否满足 HiCache invariant fact 合同。"""

    if not _is_hicache_profile_event(args):
        return
    _observe_token_references(accumulator, args)
    fact_class = str(args.get("fact_class") or "")
    model_input = _true_like(args.get("model_input"))
    dag_input = not _false_like(args.get("dag_input"))
    if fact_class != "invariant_state":
        if model_input:
            accumulator["counts"]["model_input_non_invariant_events"] += 1
        return

    accumulator["counts"]["invariant_events"] += 1
    if not model_input:
        accumulator["counts"]["invariant_without_model_input"] += 1
    if dag_input:
        accumulator["counts"]["invariant_with_dag_input"] += 1
    if str(args.get("fact_granularity") or "") != "atomic":
        accumulator["counts"]["non_atomic_invariant_events"] += 1

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
    """记录 token dictionary/span 引用，检查 span 是否有字典支撑。"""

    for value in args.values():
        if not isinstance(value, dict):
            continue
        token_path_id = value.get("token_path_id")
        if isinstance(token_path_id, str) and token_path_id:
            accumulator["dictionary_ids"].add(token_path_id)
            if isinstance(value.get("token_ids"), list):
                accumulator["dictionary_ids_with_tokens"].add(token_path_id)
        path_id = value.get("path_id")
        if isinstance(path_id, str) and path_id:
            accumulator["span_path_ids"].add(path_id)


def _missing_invariant_fields(args: dict[str, Any], role: str) -> list[str]:
    """返回某个 invariant role 缺失的必需字段列表。"""

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
    """汇总 HiCache invariant fact 合同检查结果。"""

    counts: Counter[str] = accumulator["counts"]
    missing_token_dictionary_refs = sorted(accumulator["span_path_ids"] - accumulator["dictionary_ids"])
    dictionary_ids_without_tokens = sorted(accumulator["dictionary_ids"] - accumulator["dictionary_ids_with_tokens"])
    route_error_events = (
        counts["model_input_non_invariant_events"]
        + counts["invariant_without_model_input"]
        + counts["invariant_with_dag_input"]
        + counts["non_atomic_invariant_events"]
        + counts["unknown_invariant_role_events"]
    )
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
        "route_error_events": route_error_events,
        "model_input_non_invariant_events": counts["model_input_non_invariant_events"],
        "invariant_without_model_input": counts["invariant_without_model_input"],
        "invariant_with_dag_input": counts["invariant_with_dag_input"],
        "non_atomic_invariant_events": counts["non_atomic_invariant_events"],
        "unknown_invariant_role_events": counts["unknown_invariant_role_events"],
        "token_dictionary_paths": len(accumulator["dictionary_ids"]),
        "token_dictionary_paths_with_token_ids": len(accumulator["dictionary_ids_with_tokens"]),
        "token_span_refs": len(accumulator["span_path_ids"]),
        "missing_token_dictionary_refs": missing_token_dictionary_refs,
        "dictionary_ids_without_tokens": dictionary_ids_without_tokens,
        "seq_scope_count": len(accumulator["seq_by_scope"]),
        "seq_order_error_count": seq_order_error_count,
        "ready": counts["missing_required_fact_events"] == 0
        and route_error_events == 0
        and not missing_token_dictionary_refs
        and not dictionary_ids_without_tokens
        and seq_order_error_count == 0,
    }


def _has_fact(value: Any) -> bool:
    """判断字段值是否能作为有效事实参与合同检查。"""

    if value is None:
        return False
    if isinstance(value, str):
        return bool(value)
    if isinstance(value, (list, tuple, set, dict)):
        return len(value) > 0
    return True


def _has_token_dictionary(value: Any) -> bool:
    """判断 token dictionary 是否包含模型所需的身份字段。"""

    if not isinstance(value, dict):
        return False
    return (
        isinstance(value.get("token_path_id"), str)
        and bool(value.get("token_path_id"))
        and _has_fact(value.get("token_count"))
        and _has_fact(value.get("hash_algo"))
    )


def _has_token_span(value: Any) -> bool:
    """判断 token span 是否能引用已记录 token dictionary。"""

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
    """宽松解析整数，避免 bool 被误当成 0/1。"""

    if value is None or isinstance(value, bool):
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def _is_hicache_profile_event(args: dict[str, Any]) -> bool:
    """识别需要参与 HiCache 专项质量审计的事件。"""

    target_id = str(args.get("target_id") or "").lower()
    event_role = str(args.get("event_role") or "")
    if target_id.startswith(("hiradix.", "hicache.", "hicache_controller.")):
        return True
    return event_role in _ROLE_TO_MECHANISM or event_role in _INVARIANT_REQUIRED_FIELDS_BY_ROLE


def _false_like(value: Any) -> bool:
    """解析常见 false 字符串。"""

    return str(value).lower() in {"false", "0", "no", "off"}


def _true_like(value: Any) -> bool:
    """解析常见 true 字符串。"""

    return str(value).lower() in {"true", "1", "yes", "on"}


def _new_hicache_capacity_accumulator() -> dict[str, Any]:
    """创建 validation-only capacity snapshot 累加器。"""

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
    """汇总 capacity/policy snapshot 中出现过的标量值。"""

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
    """把嵌套 capacity 对象展开成可比较的标量路径。"""

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
    """把非空观测值加入去重集合。"""

    if value is not None:
        values.add(str(value))


if __name__ == "__main__":
    raise SystemExit(main())
