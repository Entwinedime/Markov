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
    page_identity = Counter()
    mechanism_counts = Counter()
    page_identity_required = Counter()
    for event in events:
        raw_args = event.get("args")
        if not isinstance(raw_args, dict):
            continue
        _observe_page_identity(page_identity, raw_args)
        _observe_mechanism(mechanism_counts, raw_args)
        _observe_required_page_identity(page_identity_required, raw_args)
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
    missing_mechanisms = sorted(
        mechanism
        for mechanism in expected_mechanisms
        if mechanism_counts.get(mechanism, 0) <= 0
    )
    if missing_mechanisms:
        errors.append("expected_hicache_mechanisms_missing")
    if page_identity_required["required_events_missing_page_identity"] > 0:
        errors.append("stateful_hicache_page_identity_missing")

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
        "page_identity_coverage": {
            "hicache_end_events": page_identity["hicache_end_events"],
            "hicache_end_events_with_page_identity": page_identity["hicache_end_events_with_page_identity"],
            "operation_end_events": page_identity["operation_end_events"],
            "operation_end_events_with_page_identity": page_identity["operation_end_events_with_page_identity"],
            "count_only_operation_events": page_identity["operation_end_events"]
            - page_identity["operation_end_events_with_page_identity"],
            "stateful_required_events": page_identity_required["required_events"],
            "stateful_required_events_with_page_identity": page_identity_required["required_events_with_page_identity"],
            "stateful_required_events_missing_page_identity": page_identity_required[
                "required_events_missing_page_identity"
            ],
            "stateful_page_identity_ready": page_identity_required["required_events_missing_page_identity"] == 0,
        },
        "workload_report": str(workload_report) if workload_report else None,
        "expected_cache_mechanisms": expected_mechanisms,
        "observed_cache_mechanisms": dict(sorted(mechanism_counts.items())),
        "missing_cache_mechanisms": missing_mechanisms,
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


_NON_OPERATION_ROLES = {
    "",
    "lookup",
    "prefetch_decision",
    "prefetch_progress",
    "prefetch_loaded_tokens",
    "l3_hit_query",
}


_ROLE_TO_MECHANISM = {
    "lookup": "lookup",
    "prefetch_decision": "prefetch_decision",
    "prefetch_schedule": "prefetch_schedule",
    "prefetch_progress": "prefetch_progress",
    "prefetch_loaded_tokens": "prefetch_progress",
    "init_load_back": "load_back",
    "load_back": "load_back",
    "insert": "insert",
    "write_backup": "write_backup",
    "write_storage_schedule": "write_storage",
    "evict": "evict",
    "l2_to_l1_enqueue": "load_back",
    "l2_to_l1_start": "load_back",
    "l1_to_l2_enqueue": "write_backup",
    "l1_to_l2_start": "write_backup",
    "l3_prefetch_enqueue": "prefetch_schedule",
    "l3_hit_query": "prefetch_query",
    "l3_to_l2_transfer": "prefetch_transfer",
    "l2_to_l3_enqueue": "write_storage",
    "l2_to_l3_transfer": "write_storage",
}


_PAGE_IDENTITY_REQUIRED_ROLES = {
    "insert",
    "prefetch_schedule",
    "l3_prefetch_enqueue",
    "l3_to_l2_transfer",
    "load_back",
    "write_backup",
    "write_storage_schedule",
    "l2_to_l3_enqueue",
    "l2_to_l3_transfer",
    "evict",
}


def _observe_page_identity(counter: Counter[str], args: dict[str, Any]) -> None:
    """统计 HiCache 事件中 page identity 的覆盖情况。"""

    if args.get("phase") != "end":
        return
    event_role = str(args.get("event_role") or "")
    target_id = str(args.get("target_id") or "")
    if not event_role and not target_id.startswith(("hiradix.", "controller.", "scheduler.")):
        return
    counter["hicache_end_events"] += 1
    if _has_page_identity(args):
        counter["hicache_end_events_with_page_identity"] += 1
    if event_role not in _NON_OPERATION_ROLES:
        counter["operation_end_events"] += 1
        if _has_page_identity(args):
            counter["operation_end_events_with_page_identity"] += 1


def _observe_mechanism(counter: Counter[str], args: dict[str, Any]) -> None:
    if args.get("phase") != "end":
        return
    event_role = str(args.get("event_role") or "")
    mechanism = _ROLE_TO_MECHANISM.get(event_role)
    if mechanism:
        counter[mechanism] += 1


def _observe_required_page_identity(counter: Counter[str], args: dict[str, Any]) -> None:
    """只对会改变 cache resident/dirty/backuped 的事件执行严格 page 检查。

    controller start/enqueue 这类队列锚点可以是 count-only；真正状态转移必须有
    page identity，否则 HiCache 状态验证不能通过。
    """

    if args.get("phase") != "end":
        return
    event_role = str(args.get("event_role") or "")
    if event_role not in _PAGE_IDENTITY_REQUIRED_ROLES:
        return
    counter["required_events"] += 1
    if _has_page_identity(args):
        counter["required_events_with_page_identity"] += 1
    else:
        counter["required_events_missing_page_identity"] += 1


def _has_page_identity(args: dict[str, Any]) -> bool:
    value = args.get("page_identity")
    if value is None:
        return False
    if isinstance(value, list):
        return len(value) > 0
    return True


def _add_optional(values: set[str], value: Any) -> None:
    if value is not None:
        values.add(str(value))


if __name__ == "__main__":
    raise SystemExit(main())
