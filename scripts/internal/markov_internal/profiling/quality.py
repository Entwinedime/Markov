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
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


from ..common.paths import CONTAINER_REPO_PREFIXES, ROOT_DIR
from ..common.trace import load_chrome_trace_events
from ..hicache.facts import (
    HICACHE_CONSUMER_FINAL_STATE_VALIDATOR,
    HICACHE_CONSUMER_TRANSITION_VALIDATOR,
)
from .forced_tokens import forced_token_quality_from_workload_report
from .quality_hicache import (
    configured_fact_role as _configured_fact_role,
    configured_mechanisms as _configured_mechanisms,
    finalize_hicache_capacity as _finalize_hicache_capacity,
    finalize_hicache_state_facts as _finalize_hicache_state_facts,
    new_hicache_capacity_accumulator as _new_hicache_capacity_accumulator,
    new_hicache_state_fact_accumulator as _new_hicache_state_fact_accumulator,
    observe_hicache_capacity as _observe_hicache_capacity,
    observe_hicache_state_fact as _observe_hicache_state_fact,
    observe_mechanism as _observe_mechanism,
)


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
    run_config = _load_run_config(manifest, run_dir)
    expected_forced_token_mode = _expected_forced_token_mode(run_config)
    profiling = manifest.get("profiling") if isinstance(manifest.get("profiling"), dict) else {}
    trace = manifest.get("trace") if isinstance(manifest.get("trace"), dict) else {}
    sidecar = manifest.get("sidecar") if isinstance(manifest.get("sidecar"), dict) else {}

    configured_targets = _configured_targets(profiling)
    channels_enabled = {
        str(channel)
        for channel in profiling.get("channels_enabled") or []
        if isinstance(channel, str)
    }
    requested_consumers = {
        str(consumer)
        for consumer in profiling.get("python_consumers") or []
        if isinstance(consumer, str)
    }
    hicache_state_trace_enabled = bool(
        requested_consumers
        & {HICACHE_CONSUMER_FINAL_STATE_VALIDATOR, HICACHE_CONSUMER_TRANSITION_VALIDATOR}
    )
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
    lifecycle_observed_configured = any(
        _configured_fact_role(target) == "request_lifecycle_path_observed"
        for target in configured_targets.values()
    )
    mechanism_counts = Counter()
    state_fact_accumulator = _new_hicache_state_fact_accumulator()
    capacity_accumulator = _new_hicache_capacity_accumulator()
    for event in events:
        raw_args = event.get("args")
        if not isinstance(raw_args, dict):
            continue
        _observe_hicache_capacity(capacity_accumulator, raw_args)
        _observe_mechanism(mechanism_counts, raw_args)
        _observe_hicache_state_fact(state_fact_accumulator, raw_args)
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
    forced_token_quality = forced_token_quality_from_workload_report(workload_report)
    if expected_forced_token_mode:
        if workload_report is None:
            errors.append("forced_token_workload_report_missing")
        if (
            not forced_token_quality.get("enabled")
            or forced_token_quality.get("mode") != expected_forced_token_mode
        ):
            errors.append("forced_token_mode_mismatch")
    if forced_token_quality["errors"]:
        errors.extend(forced_token_quality["errors"])
    expected_mechanisms = _expected_mechanisms_from_workload(workload_report)
    expected_configured_mechanisms = sorted(set(expected_mechanisms) & set(configured_mechanisms))
    missing_mechanisms = sorted(
        mechanism
        for mechanism in expected_configured_mechanisms
        if mechanism_counts.get(mechanism, 0) <= 0
    )
    if missing_mechanisms:
        errors.append("expected_hicache_mechanisms_missing")
    state_fact_coverage = _finalize_hicache_state_facts(state_fact_accumulator)
    if state_fact_coverage["missing_required_fact_events"] > 0:
        errors.append("hicache_state_model_facts_missing")
    if state_fact_coverage["route_error_events"] > 0:
        errors.append("hicache_state_fact_route_invalid")
    if state_fact_coverage["missing_token_dictionary_refs"] or state_fact_coverage["dictionary_ids_without_tokens"]:
        errors.append("hicache_token_dictionary_missing")
    if state_fact_coverage["invalid_token_dictionary_issue_count"] > 0:
        errors.append("hicache_token_dictionary_invalid")
    if state_fact_coverage["seq_order_error_count"] > 0:
        errors.append("hicache_state_fact_seq_invalid")
    if state_fact_coverage.get("prefetch_path_contract_error_count", 0) > 0:
        errors.append("hicache_prefetch_decision_path_contract_invalid")
    if lifecycle_observed_configured and state_fact_coverage["lifecycle_path_contract_error_count"] > 0:
        errors.append("hicache_lifecycle_path_contract_invalid")
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
        "hicache_state_model_fact_coverage": state_fact_coverage,
        "workload_report": str(workload_report) if workload_report else None,
        "expected_forced_token_mode": expected_forced_token_mode,
        "forced_token_quality": forced_token_quality,
        "expected_cache_mechanisms": expected_mechanisms,
        "configured_cache_mechanisms": configured_mechanisms,
        "expected_configured_cache_mechanisms": expected_configured_mechanisms,
        "observed_cache_mechanisms": dict(sorted(mechanism_counts.items())),
        "missing_cache_mechanisms": missing_mechanisms,
        "lifecycle_observed_configured": lifecycle_observed_configured,
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
    """按 manifest requested consumers 从 target catalog 重建已配置 targets。"""

    targets: dict[str, dict[str, Any]] = {}
    requested = {
        str(consumer)
        for consumer in profiling.get("python_consumers") or []
        if isinstance(consumer, str)
    }
    if not requested:
        return targets
    catalog_path = profiling.get("python_target_catalog")
    path = map_repo_path(Path(str(catalog_path))) if isinstance(catalog_path, str) else ROOT_DIR / "configs/profiling/hicache_probe_targets.json"
    if not path.is_file():
        return targets
    raw_targets = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(raw_targets, list):
        return targets
    for item in raw_targets:
        if not isinstance(item, dict):
            continue
        fact = item.get("fact") if isinstance(item.get("fact"), dict) else {}
        consumers = fact.get("consumers") if isinstance(fact.get("consumers"), list) else []
        if not (requested & {str(consumer) for consumer in consumers if isinstance(consumer, str)}):
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


def _load_run_config(manifest: dict[str, Any], run_dir: Path) -> dict[str, Any]:
    """读取 run-local config，用于判断 workload 必须满足的输入合同。"""

    raw_path = manifest.get("config_path")
    config_path = map_repo_path(Path(str(raw_path))) if raw_path else run_dir / "config.json"
    if not config_path.is_file():
        return {}
    try:
        payload = json.loads(config_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return {}
    return payload if isinstance(payload, dict) else {}


def _expected_forced_token_mode(config: dict[str, Any]) -> str | None:
    """从当前 run config 读取不可降级的 forced-token 模式。"""

    metadata = config.get("metadata") if isinstance(config.get("metadata"), dict) else {}
    profile_mode = metadata.get("profile_mode")
    if profile_mode == "forced_token_capture":
        return "capture"
    if profile_mode == "forced_token_replay":
        return "replay"
    return None


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


def _add_optional(values: set[str], value: Any) -> None:
    """把非空观测值加入去重集合。"""

    if value is not None:
        values.add(str(value))


if __name__ == "__main__":
    raise SystemExit(main())
