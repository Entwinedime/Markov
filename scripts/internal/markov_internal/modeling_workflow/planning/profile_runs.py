"""profile run 发现、解析、筛选和 prediction matrix 构造。"""

from __future__ import annotations

import shlex
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from ...common.io import load_json
from ...common.paths import map_repo_path
from ..types import CacheStatePredictionRef, ProfileRunRef


@dataclass(frozen=True)
class ProfileRunDiscovery:
    """从 suite 目录和显式 manifest 路径发现 profile run。"""

    profile_run_dirs: tuple[Path, ...]
    manifests: tuple[Path, ...]

    def discover(self) -> list[ProfileRunRef]:
        """返回按 input/config/run 排序后的 profile run。"""

        manifest_paths: set[Path] = {path.resolve() for path in self.manifests}
        for run_dir in self.profile_run_dirs:
            manifest_paths.update(path.resolve() for path in run_dir.glob("*/profile_manifest.json"))
            direct = run_dir / "profile_manifest.json"
            if direct.is_file():
                manifest_paths.add(direct.resolve())
        parser = ProfileRunParser()
        runs = [parser.from_manifest(path) for path in sorted(manifest_paths)]
        return sorted(runs, key=lambda run: (run.input_id, run.config_id, run.run_id))


@dataclass(frozen=True)
class ProfileRunParser:
    """把 `profile_manifest.json` 转成 workflow 内部 run 描述。"""

    def from_manifest(self, manifest_path: Path) -> ProfileRunRef:
        """解析一个 profile manifest。"""

        manifest = load_json(manifest_path)
        run_dir = map_repo_path(Path(str(manifest.get("run_dir") or manifest_path.parent)))
        config_path = map_repo_path(Path(str(manifest.get("config_path") or run_dir / "config.json")))
        config = load_json(config_path)
        metadata = config.get("metadata") if isinstance(config.get("metadata"), dict) else {}
        sidecar = manifest.get("sidecar") if isinstance(manifest.get("sidecar"), dict) else {}
        return ProfileRunRef(
            manifest_path=manifest_path,
            run_dir=run_dir,
            config_path=config_path,
            run_id=first_non_empty(
                manifest.get("run_id"),
                manifest.get("experiment_id"),
                config.get("run_id"),
                config.get("name"),
                manifest_path.parent.name,
            ),
            config_id=first_non_empty(
                metadata.get("suite_server_id"),
                metadata.get("config_id"),
                config.get("id"),
                config.get("name"),
                manifest_path.parent.name,
            ),
            input_id=first_non_empty(
                metadata.get("suite_input_id"),
                metadata.get("input_id"),
                metadata.get("workload_id"),
                "default_input",
            ),
            input_class=first_non_empty(metadata.get("input_class"), "unknown"),
            python_probe_files=tuple(existing_sidecar_paths(sidecar.get("python_probe_files", []))),
            hicache_config=extract_hicache_modeling_config(config, run_dir),
        )


@dataclass(frozen=True)
class RunSelector:
    """按 CLI selector 从已发现的 run 中筛选候选集。"""

    input_ids: frozenset[str]
    config_ids: frozenset[str]
    source_config_ids: frozenset[str]
    target_config_ids: frozenset[str]

    def filter(self, runs: list[ProfileRunRef]) -> list[ProfileRunRef]:
        """返回满足 input/config selector 的 run。"""

        config_filter = self.config_ids | self.source_config_ids | self.target_config_ids
        return [
            run
            for run in runs
            if (not self.input_ids or run.input_id in self.input_ids)
            and (not config_filter or run.config_id in config_filter)
        ]


@dataclass(frozen=True)
class PredictionMatrixBuilder:
    """为同一 workload input 内的 source/target config 构造 prediction matrix。"""

    runs: list[ProfileRunRef]
    source_config_ids: frozenset[str]
    target_config_ids: frozenset[str]
    prediction_scope: frozenset[str]
    max_predictions: int = 0

    def build(self) -> list[CacheStatePredictionRef]:
        """返回稳定排序后的 prediction 列表。"""

        selected: dict[tuple[str, str, str], CacheStatePredictionRef] = {}
        for _input_id, by_config in group_runs_by_input(self.runs).items():
            sources = [
                run
                for config_id, run in by_config.items()
                if not self.source_config_ids or config_id in self.source_config_ids
            ]
            targets = [
                run
                for config_id, run in by_config.items()
                if not self.target_config_ids or config_id in self.target_config_ids
            ]
            for source in sources:
                for target in targets:
                    prediction = CacheStatePredictionRef(source=source, target=target)
                    if prediction.is_self and "self" not in self.prediction_scope:
                        continue
                    if not prediction.is_self and "cross" not in self.prediction_scope:
                        continue
                    selected[(prediction.input_id, source.config_id, target.config_id)] = prediction
        predictions = [selected[key] for key in sorted(selected)]
        if self.max_predictions > 0:
            return predictions[: self.max_predictions]
        return predictions


def first_non_empty(*values: Any) -> str:
    """返回第一个非空字符串值。"""

    for value in values:
        if isinstance(value, str) and value.strip():
            return value.strip()
    return "unknown"


def existing_sidecar_paths(entries: Any) -> list[Path]:
    """从 manifest sidecar 中提取存在的文件路径。"""

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


def extract_hicache_modeling_config(config: dict[str, Any], run_dir: Path) -> dict[str, Any] | None:
    """提取 HiCache cache-state 模型需要的目标配置。"""

    modeling = config.get("modeling") if isinstance(config.get("modeling"), dict) else {}
    hicache = modeling.get("hicache") if isinstance(modeling.get("hicache"), dict) else {}
    if not hicache:
        return None
    return apply_sglang_capacity_from_server_cmd(run_dir, {"enabled": True, **hicache})


def apply_sglang_capacity_from_server_cmd(run_dir: Path, hicache_config: dict[str, Any]) -> dict[str, Any]:
    """用 SGLang server command 对齐建模侧 capacity 配置。"""

    flags = parse_server_command_flags(run_dir / "server_cmd.txt")
    if not flags:
        return hicache_config

    page_size = parse_nonnegative_int_or_none(flags.get("page_size")) or parse_nonnegative_int_or_none(
        hicache_config.get("page_size")
    )
    max_total_tokens = parse_nonnegative_int_or_none(flags.get("max_total_tokens")) or parse_nonnegative_int_or_none(
        flags.get("max_total_num_tokens")
    )
    if not page_size or not max_total_tokens:
        return hicache_config

    result = dict(hicache_config)
    result["page_size"] = page_size
    result["l1_capacity_pages"] = max_total_tokens // page_size

    hicache_size = parse_nonnegative_int_or_none(flags.get("hicache_size")) or 0
    hicache_ratio = parse_nonnegative_float_or_none(flags.get("hicache_ratio"))
    if hicache_size <= 0 and hicache_ratio is not None and hicache_ratio > 0.0:
        host_capacity_tokens = (int(max_total_tokens * hicache_ratio) // page_size + 1) * page_size
        result["l2_capacity_pages"] = host_capacity_tokens // page_size
        prefetch_limit_tokens = max(0, int(0.8 * (host_capacity_tokens - max_total_tokens)))
        result["prefetch_capacity_limit_pages"] = prefetch_limit_tokens // page_size
    return result


def parse_server_command_flags(path: Path) -> dict[str, str]:
    """解析 `--key value` 和 `--key=value` 形式的 server 参数。"""

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


def parse_nonnegative_int_or_none(value: Any) -> int | None:
    """解析非负整数；无法解析时返回 None。"""

    try:
        if value is None:
            return None
        parsed = int(float(str(value)))
        return parsed if parsed >= 0 else None
    except (TypeError, ValueError):
        return None


def parse_nonnegative_float_or_none(value: Any) -> float | None:
    """解析非负浮点数；无法解析时返回 None。"""

    try:
        if value is None:
            return None
        parsed = float(str(value))
        return parsed if parsed >= 0.0 else None
    except (TypeError, ValueError):
        return None


def group_runs_by_input(runs: list[ProfileRunRef]) -> dict[str, dict[str, ProfileRunRef]]:
    """按 input/config 把 profile run 分组。"""

    grouped: dict[str, dict[str, ProfileRunRef]] = {}
    for run in runs:
        grouped.setdefault(run.input_id, {})[run.config_id] = run
    return {input_id: dict(sorted(by_config.items())) for input_id, by_config in sorted(grouped.items())}
