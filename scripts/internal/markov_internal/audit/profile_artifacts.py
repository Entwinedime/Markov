#!/usr/bin/env python3
"""通用 profiling 后 artifact 审计。

该模块只检查 profiling run 结束后留下的文件、trace channel 和 Python probe
target 命中情况。它不判断某个后端模型是否可以消费该 run；consumer-specific
readiness 归属 `markov_internal.hicache.quality` 等领域模块。
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from ..common.paths import ROOT_DIR, map_repo_path
from ..common.trace import load_chrome_trace_events


@dataclass
class TargetArtifactAudit:
    """单个已配置 Python probe target 的命中与 required field 摘要。"""

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
        """累计一条 Python probe event payload。"""

        self.events_total += 1
        self.phases[str(args.get("phase") or "unknown")] += 1
        self.statuses[str(args.get("status") or "unknown")] += 1
        for field_name in args.get("missing_required_fields") or []:
            self.missing_required_fields[str(field_name)] += 1
        for field_name, value in args.items():
            if value is not None and field_name not in {"missing_required_fields"}:
                self.field_presence[field_name] += 1
        _add_optional(self.request_ids, args.get("request_id"))
        _add_optional(self.operation_ids, args.get("operation_id"))
        _add_optional(self.node_ids, args.get("node_id"))
        _add_optional(self.node_ids, args.get("last_host_node_id"))
        _add_optional(self.node_ids, args.get("last_device_node_id"))
        _add_optional(self.node_ids, args.get("best_match_node_id"))

    def to_dict(self) -> dict[str, Any]:
        """将 Counter 和 set 序列化为稳定 JSON 值。"""

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
    """通用 profile artifact audit 的 CLI 入口。"""

    args = parse_args(argv)
    manifest_path = resolve_path(args.manifest)
    result = audit_profile_artifacts(manifest_path)
    output_path = resolve_output_path(args.output, manifest_path, result)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    sys.stdout.write(str(output_path) + "\n")
    return 0 if result.get("artifact_ready") else 1


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """解析通用 profile artifact audit 参数。"""

    parser = argparse.ArgumentParser(description="Audit generic profiling artifacts.")
    parser.add_argument("--manifest", required=True, help="profile_manifest.json path")
    parser.add_argument(
        "--output",
        help="output JSON path; defaults to run_dir/profile_artifact_audit.json",
    )
    return parser.parse_args(argv)


def audit_profile_artifacts(manifest_path: Path) -> dict[str, Any]:
    """审计文件、trace channel 和已配置 Python probe target 命中。"""

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    run_dir = map_repo_path(Path(str(manifest.get("run_dir") or manifest_path.parent)))
    profiling = manifest.get("profiling") if isinstance(manifest.get("profiling"), dict) else {}
    trace = manifest.get("trace") if isinstance(manifest.get("trace"), dict) else {}
    sidecar = manifest.get("sidecar") if isinstance(manifest.get("sidecar"), dict) else {}

    configured = configured_targets(profiling)
    channels_enabled = {str(channel) for channel in profiling.get("channels_enabled") or [] if isinstance(channel, str)}
    python_channel_enabled = "python" in channels_enabled or bool(configured)
    target_audits = {
        target_id: TargetArtifactAudit(
            target_id=target_id,
            configured=True,
            target=str(target.get("target") or ""),
        )
        for target_id, target in configured.items()
    }
    python_probe_files = existing_paths(sidecar.get("python_probe_files", []))
    events = load_python_probe_events(python_probe_files)
    unknown_targets: dict[str, TargetArtifactAudit] = {}
    for event in events:
        args = event.get("args")
        if not isinstance(args, dict):
            continue
        target_id = str(args.get("target_id") or "unknown")
        target = target_audits.get(target_id)
        if target is None:
            target = unknown_targets.setdefault(
                target_id,
                TargetArtifactAudit(target_id=target_id, configured=False),
            )
        target.observe(args)

    missing_targets = sorted(target_id for target_id, audit in target_audits.items() if audit.events_total == 0)
    targets_with_missing_fields = sorted(
        target_id for target_id, audit in target_audits.items() if audit.missing_required_fields
    )
    exception_targets = sorted(
        target_id
        for target_id, audit in {**target_audits, **unknown_targets}.items()
        if audit.phases.get("exception", 0) > 0 or audit.statuses.get("exception", 0) > 0
    )

    artifact_errors: list[str] = []
    if python_channel_enabled and not python_probe_files:
        artifact_errors.append("missing_python_probe_files")
    if configured and len(missing_targets) == len(configured):
        artifact_errors.append("all_python_probe_targets_missing")
    if targets_with_missing_fields:
        artifact_errors.append("python_probe_required_fields_missing")
    if exception_targets:
        artifact_errors.append("python_probe_exception_events")

    torch_files = existing_paths(trace.get("torch_trace_files", []))
    if not torch_files:
        torch_files = existing_dir_files(trace.get("torch_trace_dir"), "**/trace_view.json")

    return {
        "schema": "trace_sim.profile_artifact_audit.v1",
        "manifest_path": str(manifest_path),
        "run_dir": str(run_dir),
        "profiling_ready": bool(manifest.get("profiling_ready")),
        "status": manifest.get("status"),
        "dry_run": bool(manifest.get("dry_run")),
        "trace_files": {
            "torch": len(torch_files),
            "ld_preload": len(existing_paths(trace.get("ld_preload_trace_files", []))),
            "python_probe": len(python_probe_files),
        },
        "python_probe_events": len(events),
        "configured_target_count": len(configured),
        "observed_target_count": sum(1 for audit in target_audits.values() if audit.events_total > 0),
        "missing_targets": missing_targets,
        "targets_with_missing_required_fields": targets_with_missing_fields,
        "exception_targets": exception_targets,
        "unknown_targets": sorted(unknown_targets),
        "targets": {target_id: audit.to_dict() for target_id, audit in sorted(target_audits.items())},
        "unknown_target_details": {target_id: audit.to_dict() for target_id, audit in sorted(unknown_targets.items())},
        "artifact_errors": artifact_errors,
        "artifact_ready": not artifact_errors,
    }


def configured_targets(profiling: dict[str, Any]) -> dict[str, dict[str, Any]]:
    """按 requested consumers 重建本次应配置的 Python probe targets。"""

    targets: dict[str, dict[str, Any]] = {}
    requested = {str(consumer) for consumer in profiling.get("python_consumers") or [] if isinstance(consumer, str)}
    if not requested:
        return targets
    catalog_path = profiling.get("python_target_catalog")
    path = (
        map_repo_path(Path(str(catalog_path)))
        if isinstance(catalog_path, str)
        else ROOT_DIR / "configs/profiling/hicache_probe_targets.json"
    )
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


def existing_paths(entries: Any) -> list[Path]:
    """从 manifest sidecar path 条目中返回当前可读文件。"""

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


def existing_dir_files(raw_dir: Any, pattern: str) -> list[Path]:
    """从 manifest 目录字段中发现当前可读文件。"""

    if not isinstance(raw_dir, str):
        return []
    directory = map_repo_path(Path(raw_dir))
    if not directory.is_dir():
        return []
    return sorted(item for item in directory.glob(pattern) if item.is_file())


def discover_workload_report(run_dir: Path) -> Path | None:
    """查找 profiling run 产出的 workload report。"""

    if not run_dir.is_dir():
        return None
    candidates = sorted(run_dir.glob("bench/**/workload_report.json"))
    return candidates[-1] if candidates else None


def load_run_config(manifest: dict[str, Any], run_dir: Path) -> dict[str, Any]:
    """读取 run-local expanded config；文件缺失或 JSON 非法时返回空对象。"""

    raw_path = manifest.get("config_path")
    config_path = map_repo_path(Path(str(raw_path))) if raw_path else run_dir / "config.json"
    if not config_path.is_file():
        return {}
    try:
        payload = json.loads(config_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return {}
    return payload if isinstance(payload, dict) else {}


def load_python_probe_events(paths: list[Path]) -> list[dict[str, Any]]:
    """从可读 sidecar 文件加载所有 Python probe Chrome trace event。"""

    events: list[dict[str, Any]] = []
    for path in paths:
        raw_events, _status = load_chrome_trace_events(path, auto_repair=True)
        for event in raw_events:
            if is_python_probe_trace_event(event):
                events.append(event)
    return events


def is_python_probe_trace_event(event: dict[str, Any]) -> bool:
    """判断 Chrome trace event 是否属于 Python probe channel。"""

    args = event.get("args") if isinstance(event.get("args"), dict) else {}
    return (
        event.get("cat") == "python_probe"
        or str(args.get("domain") or "") == "python_probe"
        or str(event.get("name") or "").startswith("hicache_")
    )


def resolve_path(value: str) -> Path:
    """解析 CLI 路径，必要时从 repo-relative 形式转成绝对路径。"""

    path = Path(value).expanduser()
    if path.is_absolute():
        return map_repo_path(path)
    return ROOT_DIR / path


def resolve_output_path(value: str | None, manifest_path: Path, result: dict[str, Any]) -> Path:
    """解析通用 artifact audit 输出路径。"""

    if value:
        return resolve_path(value)
    run_dir = Path(str(result.get("run_dir") or manifest_path.parent))
    return run_dir / "profile_artifact_audit.json"


def _add_optional(values: set[str], value: Any) -> None:
    """把非空值加入字符串集合。"""

    if value is not None:
        values.add(str(value))


if __name__ == "__main__":
    raise SystemExit(main())
