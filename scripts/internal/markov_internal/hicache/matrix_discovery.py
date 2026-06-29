"""HiCache 矩阵使用的 profile run 发现与 prediction spec 展开。"""

from __future__ import annotations

import re
import shlex
from pathlib import Path
from typing import Any

from ..common.io import load_json
from ..common.paths import map_repo_path
from .matrix_types import PredictionSpec, ProfileRun


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
    run_id = str(
        manifest.get("run_id")
        or manifest.get("experiment_id")
        or config.get("run_id")
        or config.get("name")
        or manifest_path.parent.name
    )
    config_id = first_non_empty(
        metadata.get("suite_server_id"),
        infer_config_id(run_id),
    )
    input_id = first_non_empty(
        metadata.get("suite_input_id"),
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
    """用 SGLang server command 中的真实 pool 参数修正建模容量。"""

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
