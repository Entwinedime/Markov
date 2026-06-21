#!/usr/bin/env python3
"""HiCache state matrix validation 的共享编排逻辑。"""

from __future__ import annotations

import collections
import hashlib
import json
import re
import shlex
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from hicache_state_cross_input_audit import canonical_json, extract_audit_events
from profile_quality import audit_profile, map_repo_path
from trace_json import TraceLoadStatus, load_chrome_trace_events


ROOT_DIR = Path(__file__).resolve().parents[2]
ACTIVE_STATE_TIERS = (
    "l1_resident_pages",
    "l2_resident_pages",
    "dirty_pages",
    "backuped_pages",
    "evicted_pages",
    "locked_pages",
)
WORKLOAD_SIGNATURE_ROLES = (
    "request_admission",
    "request_bound_match_anchor",
    "request_lifecycle_anchor",
)


@dataclass(frozen=True)
class ProfileRun:
    """矩阵中的一次真实 profiling run。"""

    manifest_path: Path
    run_dir: Path
    config_path: Path
    run_id: str
    config_id: str
    input_id: str
    input_class: str
    hicache_config: dict[str, Any]
    python_probe_files: tuple[Path, ...]

    @property
    def prediction_slug(self) -> str:
        """用于输出目录名的稳定短标签。"""

        return f"{safe_slug(self.config_id)}__{safe_slug(self.input_id)}"


@dataclass(frozen=True)
class PredictionSpec:
    """一个 source profile 到 target config/oracle 的 state prediction。"""

    source: ProfileRun
    target: ProfileRun

    @property
    def input_id(self) -> str:
        """当前 prediction 所属 workload input。"""

        return self.source.input_id

    @property
    def is_self(self) -> bool:
        """是否为同配置 self prediction。"""

        return self.source.config_id == self.target.config_id

    @property
    def label(self) -> str:
        """矩阵格子的稳定标签。"""

        return f"{self.input_id}/{self.source.config_id}->{self.target.config_id}"


def load_json(path: Path) -> Any:
    """读取 JSON 文件。"""

    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, value: Any) -> None:
    """写出稳定 JSON 文件。"""

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def safe_slug(value: str) -> str:
    """把矩阵 id 转成可用作文件/目录名的 slug。"""

    return re.sub(r"[^A-Za-z0-9_.-]+", "_", value.strip()).strip("._") or "unknown"


def discover_profile_runs(profile_run_dirs: list[Path], manifests: list[Path]) -> list[ProfileRun]:
    """从 run dir 或显式 manifest 列表中发现矩阵 profile。"""

    manifest_paths: set[Path] = {path.resolve() for path in manifests}
    for run_dir in profile_run_dirs:
        manifest_paths.update(path.resolve() for path in run_dir.glob("*/profile_manifest.json"))
        direct = run_dir / "profile_manifest.json"
        if direct.is_file():
            manifest_paths.add(direct.resolve())

    runs = [profile_run_from_manifest(path) for path in sorted(manifest_paths)]
    return sorted(runs, key=lambda run: (run.input_id, run.config_id, run.run_id))


def profile_run_from_manifest(manifest_path: Path) -> ProfileRun:
    """从 profile_manifest.json 和 run-local config.json 构造 ProfileRun。"""

    manifest = load_json(manifest_path)
    run_dir = map_repo_path(Path(str(manifest.get("run_dir") or manifest_path.parent)))
    config_path = map_repo_path(Path(str(manifest.get("config_path") or run_dir / "config.json")))
    config = load_json(config_path)
    metadata = config.get("metadata") if isinstance(config.get("metadata"), dict) else {}
    run_id = str(manifest.get("run_id") or manifest.get("experiment_id") or config.get("run_id") or config.get("name") or manifest_path.parent.name)
    config_id = first_non_empty(
        metadata.get("suite_server_id"),
        metadata.get("mainline_one_scenario"),
        infer_config_id(run_id),
    )
    input_id = first_non_empty(
        metadata.get("suite_input_id"),
        metadata.get("mainline_one_input"),
        infer_input_id(run_id),
    )
    input_class = first_non_empty(metadata.get("input_class"), "unknown")
    hicache_cfg = extract_hicache_modeling_config(config)
    hicache_cfg = apply_sglang_capacity_from_server_cmd(run_dir, hicache_cfg)
    sidecar = manifest.get("sidecar") if isinstance(manifest.get("sidecar"), dict) else {}
    python_probe_files = tuple(existing_sidecar_paths(sidecar.get("python_probe_files", [])))
    return ProfileRun(
        manifest_path=manifest_path,
        run_dir=run_dir,
        config_path=config_path,
        run_id=run_id,
        config_id=config_id,
        input_id=input_id,
        input_class=input_class,
        hicache_config=hicache_cfg,
        python_probe_files=python_probe_files,
    )


def first_non_empty(*values: Any) -> str:
    """返回第一个非空字符串。"""

    for value in values:
        if isinstance(value, str) and value.strip():
            return value.strip()
    return "unknown"


def infer_config_id(run_id: str) -> str:
    """从旧 run_id 中尽力推导 config id。"""

    text = re.sub(r"^\d+_", "", run_id)
    for marker in ("_manual_", "_bench_"):
        if marker in text:
            return text.split(marker, 1)[0]
    parts = text.split("_")
    return parts[0] if parts else "unknown"


def infer_input_id(run_id: str) -> str:
    """从旧 run_id 中尽力推导 input id。"""

    text = re.sub(r"^\d+_", "", run_id)
    for marker in ("_manual_", "_bench_"):
        if marker in text:
            kind, rest = marker.strip("_").split("_")[0], text.split(marker, 1)[1]
            return f"{kind}_{rest}"
    return "unknown"


def extract_hicache_modeling_config(config: dict[str, Any]) -> dict[str, Any]:
    """读取 run-local modeling.hicache target 配置。"""

    modeling = config.get("modeling") if isinstance(config.get("modeling"), dict) else {}
    hicache = modeling.get("hicache") if isinstance(modeling.get("hicache"), dict) else {}
    if not hicache:
        raise ValueError(f"missing modeling.hicache in {config.get('name') or config.get('id')}")
    return {"enabled": True, **hicache}


def apply_sglang_capacity_from_server_cmd(run_dir: Path, hicache_config: dict[str, Any]) -> dict[str, Any]:
    """用 SGLang server command 中的真实 pool 参数修正建模容量。

    run-local config 里的 capacity 可能是早期手写 effective budget。SGLang 实际
    host pool 使用 `device_pool.size * hicache_ratio`，再按 page 对齐并额外多分
    一个 page；prefetch rate limit 使用 host/device token capacity 差值的 0.8。
    这些是 target 配置事实，应优先于旧的建模估算。
    """

    flags = parse_server_command_flags(run_dir / "server_cmd.txt")
    if not flags:
        return hicache_config

    page_size = optional_int(flags.get("page_size")) or optional_int(hicache_config.get("page_size"))
    max_total_tokens = optional_int(flags.get("max_total_tokens")) or optional_int(flags.get("max_total_num_tokens"))
    if not page_size or not max_total_tokens:
        return hicache_config

    result = dict(hicache_config)
    result["page_size"] = page_size
    result["l1_capacity_pages"] = max_total_tokens // page_size

    hicache_size = optional_int(flags.get("hicache_size")) or 0
    hicache_ratio = optional_float(flags.get("hicache_ratio"))
    if hicache_size <= 0 and hicache_ratio is not None and hicache_ratio > 0.0:
        host_capacity_tokens = (int(max_total_tokens * hicache_ratio) // page_size + 1) * page_size
        result["l2_capacity_pages"] = host_capacity_tokens // page_size
        prefetch_limit_tokens = max(0, int(0.8 * (host_capacity_tokens - max_total_tokens)))
        result["prefetch_capacity_limit_pages"] = prefetch_limit_tokens // page_size
    return result


def parse_server_command_flags(path: Path) -> dict[str, str]:
    """解析 `server_cmd.txt` 中的 `--k v` / `--k=v` 参数。"""

    if not path.is_file():
        return {}
    try:
        tokens = shlex.split(path.read_text(encoding="utf-8"))
    except ValueError:
        return {}

    flags: dict[str, str] = {}
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if not token.startswith("--"):
            index += 1
            continue
        key_value = token[2:]
        if "=" in key_value:
            key, value = key_value.split("=", 1)
            flags[key.replace("-", "_")] = value
            index += 1
            continue
        key = key_value.replace("-", "_")
        if index + 1 < len(tokens) and not tokens[index + 1].startswith("--"):
            flags[key] = tokens[index + 1]
            index += 2
        else:
            flags[key] = "true"
            index += 1
    return flags


def optional_int(value: Any) -> int | None:
    """宽松解析非负整数。"""

    try:
        if value is None:
            return None
        parsed = int(float(str(value)))
        return parsed if parsed >= 0 else None
    except (TypeError, ValueError):
        return None


def optional_float(value: Any) -> float | None:
    """宽松解析非负浮点数。"""

    try:
        if value is None:
            return None
        parsed = float(str(value))
        return parsed if parsed >= 0.0 else None
    except (TypeError, ValueError):
        return None


def existing_sidecar_paths(entries: Any) -> list[Path]:
    """从 manifest sidecar 条目中筛出真实存在的 Python probe 文件。"""

    paths: list[Path] = []
    if not isinstance(entries, list):
        return paths
    for entry in entries:
        raw = entry.get("path") if isinstance(entry, dict) and entry.get("exists", True) else entry
        if not isinstance(raw, str):
            continue
        path = map_repo_path(Path(raw))
        if path.is_file():
            paths.append(path)
    return sorted(paths)


def filter_runs(
    runs: list[ProfileRun],
    *,
    input_ids: set[str],
    source_config_ids: set[str],
    target_config_ids: set[str],
) -> list[ProfileRun]:
    """按 CLI 过滤 profile runs；source/target 过滤取并集，避免提前丢 target oracle。"""

    config_filter = source_config_ids | target_config_ids
    return [
        run
        for run in runs
        if (not input_ids or run.input_id in input_ids)
        and (not config_filter or run.config_id in config_filter)
    ]


def group_runs_by_input(runs: list[ProfileRun]) -> dict[str, dict[str, ProfileRun]]:
    """按 input/config 建立矩阵索引。"""

    grouped: dict[str, dict[str, ProfileRun]] = {}
    for run in runs:
        grouped.setdefault(run.input_id, {})[run.config_id] = run
    return {input_id: dict(sorted(by_config.items())) for input_id, by_config in sorted(grouped.items())}


def build_prediction_specs(
    runs: list[ProfileRun],
    *,
    source_config_ids: set[str],
    target_config_ids: set[str],
    include_self: bool,
) -> list[PredictionSpec]:
    """构造同 input 内的 source x target prediction 矩阵。"""

    specs: list[PredictionSpec] = []
    for _input_id, by_config in group_runs_by_input(runs).items():
        sources = [
            run
            for config_id, run in by_config.items()
            if not source_config_ids or config_id in source_config_ids
        ]
        targets = [
            run
            for config_id, run in by_config.items()
            if not target_config_ids or config_id in target_config_ids
        ]
        for source in sources:
            for target in targets:
                spec = PredictionSpec(source=source, target=target)
                if include_self or not spec.is_self:
                    specs.append(spec)
    return specs


def build_quality_report(runs: list[ProfileRun], output_dir: Path) -> dict[str, Any]:
    """执行阶段一 profile quality gate，并写出 per-run 质量详情。"""

    rows: list[dict[str, Any]] = []
    signature_by_input: dict[str, dict[str, Any]] = {}
    quality_dir = output_dir / "quality"
    for run in runs:
        profile_quality_path = quality_dir / f"{safe_slug(run.run_id)}.profile_quality.json"
        if profile_quality_path.is_file():
            profile_quality = load_json(profile_quality_path)
        else:
            profile_quality = audit_profile(run.manifest_path)
            write_json(profile_quality_path, profile_quality)
        trace_summary = summarize_profile_trace(run)
        workload_signature = build_workload_signature(run)
        rows.append(
            {
                "run_id": run.run_id,
                "config_id": run.config_id,
                "input_id": run.input_id,
                "input_class": run.input_class,
                "manifest_path": str(run.manifest_path),
                "run_dir": str(run.run_dir),
                "python_probe_files": [str(path) for path in run.python_probe_files],
                "python_probe_file_count": len(run.python_probe_files),
                "trace_load_status": trace_summary["trace_load_status"],
                "invariant_event_count": trace_summary["fact_class_counts"].get("invariant_state", 0),
                "oracle_snapshot_count": trace_summary["oracle_snapshot_count"],
                "source_actual_count": trace_summary["fact_class_counts"].get("source_actual", 0),
                "timing_count": trace_summary["fact_class_counts"].get("timing_observation", 0),
                "fact_class_counts": trace_summary["fact_class_counts"],
                "missing_required_fields": profile_quality.get("hicache_invariant_coverage", {}).get("missing_fields", {}),
                "canonical_request_count": workload_signature["request_event_count"],
                "canonical_workload_signature": workload_signature["signature"],
                "canonical_workload_ready": workload_signature["ready"],
                "profile_quality_ready": bool(profile_quality.get("quality_ready")),
                "profile_quality_errors": profile_quality.get("quality_errors", []),
                "state_quality_ready": state_quality_ready(profile_quality, trace_summary, workload_signature),
                "hicache_invariant_coverage": profile_quality.get("hicache_invariant_coverage", {}),
                "hicache_capacity": profile_quality.get("hicache_capacity", {}),
                "workload_signature_detail": workload_signature,
            }
        )

    by_input: dict[str, list[dict[str, Any]]] = collections.defaultdict(list)
    for row in rows:
        by_input[str(row["input_id"])].append(row)
    for input_id, input_rows in sorted(by_input.items()):
        signatures = sorted({str(row["canonical_workload_signature"]) for row in input_rows if row["canonical_workload_signature"]})
        signature_by_input[input_id] = {
            "input_id": input_id,
            "run_count": len(input_rows),
            "config_ids": sorted(str(row["config_id"]) for row in input_rows),
            "signature_count": len(signatures),
            "signature_match": len(signatures) == 1,
            "signatures": signatures,
        }

    quality_ready = all(row["state_quality_ready"] for row in rows) and all(row["signature_match"] for row in signature_by_input.values())
    report = {
        "schema": "trace_sim.hicache.state_matrix.profile_quality.v1",
        "stage": "quality",
        "run_count": len(rows),
        "config_ids": sorted({run.config_id for run in runs}),
        "input_ids": sorted({run.input_id for run in runs}),
        "quality_ready": quality_ready,
        "state_quality_ready_count": sum(1 for row in rows if row["state_quality_ready"]),
        "profile_quality_ready_count": sum(1 for row in rows if row["profile_quality_ready"]),
        "input_workload_signatures": signature_by_input,
        "runs": rows,
        "note": (
            "quality_ready uses state modeling requirements: invariant facts, token dictionary/span, oracle snapshots, "
            "and same-input canonical workload signatures. profile_quality_ready keeps the stricter collection audit for "
            "source_actual/timing diagnostics."
        ),
    }
    write_json(output_dir / "profile_quality_5x4.json", report)
    return report


def summarize_profile_trace(run: ProfileRun) -> dict[str, Any]:
    """汇总一个 profile 的 fact class 和 oracle snapshot 覆盖情况。"""

    fact_class_counts: collections.Counter[str] = collections.Counter()
    event_role_counts: collections.Counter[str] = collections.Counter()
    oracle_snapshot_count = 0
    statuses: list[TraceLoadStatus] = []
    for path in run.python_probe_files:
        events, status = load_chrome_trace_events(path, auto_repair=True)
        statuses.append(status)
        for event in events:
            args = event.get("args") if isinstance(event.get("args"), dict) else {}
            fact_class = str(args.get("fact_class") or "")
            if fact_class:
                fact_class_counts[fact_class] += 1
            role = str(args.get("event_role") or "")
            if role:
                event_role_counts[role] += 1
            if str(args.get("event_kind") or "") == "state_snapshot":
                oracle_snapshot_count += 1
    return {
        "fact_class_counts": dict(sorted(fact_class_counts.items())),
        "event_role_counts": dict(sorted(event_role_counts.items())),
        "oracle_snapshot_count": oracle_snapshot_count,
        "trace_load_status": [status.to_dict() for status in statuses],
    }


def build_workload_signature(run: ProfileRun) -> dict[str, Any]:
    """基于 atomic invariant fact 构造同 input 可比较的 workload 签名。"""

    roles = set(WORKLOAD_SIGNATURE_ROLES)
    events, unknown_roles, unmapped_requests = extract_audit_events(list(run.python_probe_files), run.run_id, roles)
    by_role: dict[str, collections.Counter[str]] = {role: collections.Counter() for role in roles}
    for event in events:
        by_role.setdefault(event.role, collections.Counter())[event.signature] += 1
    payload = {
        "roles": {
            role: [
                {"signature": signature, "count": count}
                for signature, count in sorted(counter.items())
            ]
            for role, counter in sorted(by_role.items())
        }
    }
    encoded = canonical_json(payload)
    signature = "sha256_json:" + hashlib.sha256(encoded.encode("utf-8")).hexdigest()
    unknown = dict(sorted(unknown_roles.items()))
    unmapped = dict(sorted(unmapped_requests.items()))
    return {
        "signature": signature,
        "ready": bool(events) and not unknown and not unmapped,
        "request_event_count": len(events),
        "roles": sorted(roles),
        "role_counts": {role: sum(counter.values()) for role, counter in sorted(by_role.items())},
        "unknown_invariant_roles": unknown,
        "unmapped_request_id_events": unmapped,
    }


def state_quality_ready(profile_quality: dict[str, Any], trace_summary: dict[str, Any], workload_signature: dict[str, Any]) -> bool:
    """判断 profile 是否满足 final-state 建模输入要求。"""

    invariant = profile_quality.get("hicache_invariant_coverage")
    if not isinstance(invariant, dict) or not invariant.get("ready", False):
        return False
    if trace_summary.get("oracle_snapshot_count", 0) <= 0:
        return False
    if trace_summary.get("fact_class_counts", {}).get("invariant_state", 0) <= 0:
        return False
    return bool(workload_signature.get("ready"))


def write_target_model_config(target: ProfileRun, config_dir: Path) -> Path:
    """为 target config 写出一次可复用的 modeling config。"""

    path = config_dir / f"target_{safe_slug(target.config_id)}.json"
    payload = {
        "metadata": {
            "purpose": "Generated by hicache_state_matrix_validation.py for matrix final-state prediction.",
            "target_config_id": target.config_id,
            "target_run_id": target.run_id,
            "target_config_path": str(target.config_path),
        },
        "input": {},
        "output_dir": "data/modeling_runs/hicache_state_matrix/generated",
        "mode": "cache_state",
        "validation": {
            "hicache_state": {
                "enabled": True,
                "require_oracle_state_trace": True,
                "oracle_page_key_mode": "strip_scope",
                "oracle_trace_paths": [],
            }
        },
        "modules": [
            {
                "name": "HiCacheModule",
                "enabled": True,
                "config": {
                    "hicache": target.hicache_config,
                },
            }
        ],
        "outputs": {
            "emit_dag_chrome_trace": False,
            "emit_module_summary": True,
            "emit_validation": True,
            "debug": False,
        },
    }
    write_json(path, payload)
    return path


def prediction_output_dir(output_dir: Path, spec: PredictionSpec) -> Path:
    """返回一个 prediction 格子的输出目录。"""

    return (
        output_dir
        / "predictions"
        / safe_slug(spec.input_id)
        / f"{safe_slug(spec.source.config_id)}__to__{safe_slug(spec.target.config_id)}"
    )


def summarize_prediction(validation_path: Path) -> dict[str, Any]:
    """从 validation.json 中提取矩阵汇总字段。"""

    if not validation_path.is_file():
        return {
            "validation_ready": False,
            "validation_errors": ["missing_validation_json"],
            "hicache_state": {},
        }
    validation = load_json(validation_path)
    hicache = validation.get("hicache_state") if isinstance(validation.get("hicache_state"), dict) else {}
    return {
        "validation_ready": bool(validation.get("validation_ready")),
        "validation_errors": validation.get("validation_errors", []),
        "dag": validation.get("dag", {}),
        "e2e": validation.get("e2e", {}),
        "hicache_state": {
            "state_trace_ready": hicache.get("state_trace_ready"),
            "state_trace_events": hicache.get("state_trace_events"),
            "model_transition_events": hicache.get("model_transition_events"),
            "invariant_coverage_ready": hicache.get("invariant_coverage_ready"),
            "missing_invariant_facts": hicache.get("missing_invariant_facts", []),
            "missing_invariant_fact_counts": hicache.get("missing_invariant_fact_counts", {}),
            "non_invariant_fact_usage": hicache.get("non_invariant_fact_usage", []),
            "non_invariant_fact_usage_by_role": hicache.get("non_invariant_fact_usage_by_role", {}),
            "final_state_match": hicache.get("final_state_match"),
            "raw_final_state_match": hicache.get("raw_final_state_match"),
            "normalized_model_final_state_counts": hicache.get("normalized_model_final_state_counts", {}),
            "normalized_oracle_final_state_counts": hicache.get("normalized_oracle_final_state_counts", {}),
            "sets_diff_by_tier": hicache.get("sets_diff_by_tier", {}),
            "first_mismatch": hicache.get("first_mismatch"),
            "capacity_config_audit": hicache.get("capacity_config_audit", {}),
            "predicted_state_trace_path": hicache.get("predicted_state_trace_path"),
        },
    }


def matrix_summary(rows: list[dict[str, Any]], *, schema: str, stage: str) -> dict[str, Any]:
    """汇总 self/cross prediction rows。"""

    pass_rows = [row for row in rows if row.get("hicache_state", {}).get("final_state_match") is True]
    ready_rows = [row for row in rows if row.get("validation_ready")]
    invariant_ready_rows = [
        row
        for row in rows
        if row.get("hicache_state", {}).get("invariant_coverage_ready") is True
        and not row.get("hicache_state", {}).get("non_invariant_fact_usage")
    ]
    by_input: dict[str, dict[str, Any]] = {}
    for input_id in sorted({str(row.get("input_id")) for row in rows}):
        input_rows = [row for row in rows if row.get("input_id") == input_id]
        by_input[input_id] = {
            "prediction_count": len(input_rows),
            "final_state_match_count": sum(1 for row in input_rows if row.get("hicache_state", {}).get("final_state_match") is True),
            "validation_ready_count": sum(1 for row in input_rows if row.get("validation_ready")),
        }
    return {
        "schema": schema,
        "stage": stage,
        "prediction_count": len(rows),
        "validation_ready_count": len(ready_rows),
        "invariant_ready_count": len(invariant_ready_rows),
        "final_state_match_count": len(pass_rows),
        "final_state_pass_rate": len(pass_rows) / len(rows) if rows else None,
        "by_input": by_input,
        "predictions": rows,
    }


def tier_count_delta(row: dict[str, Any], tier: str) -> int | None:
    """计算某个 tier 的 model/oracle count delta。"""

    hicache = row.get("hicache_state") if isinstance(row.get("hicache_state"), dict) else {}
    model = hicache.get("normalized_model_final_state_counts") if isinstance(hicache.get("normalized_model_final_state_counts"), dict) else {}
    oracle = hicache.get("normalized_oracle_final_state_counts") if isinstance(hicache.get("normalized_oracle_final_state_counts"), dict) else {}
    if tier not in model and tier not in oracle:
        return None
    return int(model.get(tier, 0) or 0) - int(oracle.get(tier, 0) or 0)
