"""Profile discovery and parsing for prediction inputs or independent target scoring."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from ...common.io import load_json
from ...common.manifest import manifest_files, profile_labels
from ...common.paths import map_repo_path, repo_relative_path
from ...common.commands import command_tokens
from ..types import ProfileRunRef


def discover_profile_runs(
    manifests: tuple[Path, ...], *, profile_run_dirs: tuple[Path, ...] = ()
) -> list[ProfileRunRef]:
    """Parse explicit profiles; directory selection is for independent evaluation."""

    manifest_paths = {path.resolve() for path in manifests}
    for run_dir in profile_run_dirs:
        manifest_paths.update(path.resolve() for path in run_dir.glob("*/profile_manifest.json"))
        direct = run_dir / "profile_manifest.json"
        if direct.is_file():
            manifest_paths.add(direct.resolve())
    runs = [parse_profile_run(path) for path in sorted(manifest_paths)]
    return sorted(runs, key=lambda run: (run.input_id, run.config_id, run.run_id))


def parse_profile_run(manifest_path: Path) -> ProfileRunRef:
    """Parse a manifest and its referenced profiling configuration."""

    manifest = load_json(manifest_path)
    run_dir = map_repo_path(Path(str(manifest["run_dir"])))
    config_path = map_repo_path(Path(str(manifest["config_path"])))
    config = load_json(config_path)
    framework = str(manifest.get("framework") or config.get("framework") or "sglang")
    if framework != "sglang":
        raise ValueError(f"{framework} supports framework-neutral build-dag, not HiCache prediction")
    run_id = str(manifest.get("run_id") or repo_relative_path(manifest_path.parent))
    # Suite labels identify matrix axes. Ordinary profiles need no matrix;
    # explicit labels allow multiple workloads to share one base identity.
    name = str(config.get("name") or run_id)
    config_id, input_id = profile_labels(config, name)
    sidecar = manifest.get("sidecar") if isinstance(manifest.get("sidecar"), dict) else {}
    return ProfileRunRef(
        manifest_path=manifest_path,
        run_dir=run_dir,
        config_path=config_path,
        run_id=run_id,
        config_id=config_id,
        input_id=input_id,
        python_probe_files=tuple(manifest_files(sidecar.get("python_probe_files", []))),
        hicache_config=extract_hicache_modeling_config(config, run_dir),
    )


def extract_hicache_modeling_config(config: dict[str, Any], run_dir: Path) -> dict[str, Any] | None:
    """Extract the target HiCache model configuration from a profile config."""

    modeling = config.get("modeling") if isinstance(config.get("modeling"), dict) else {}
    hicache = modeling.get("hicache") if isinstance(modeling.get("hicache"), dict) else {}
    if hicache:
        return apply_sglang_capacity_from_server_cmd(run_dir, {"enabled": True, **hicache})
    return extract_hicache_from_server_cmd(run_dir)


def extract_hicache_from_server_cmd(run_dir: Path) -> dict[str, Any] | None:
    """Reconstruct the narrow target contract from the exact profiled server command."""

    path = run_dir / "server_cmd.txt"
    return extract_hicache_from_server_command(path.read_text(encoding="utf-8")) if path.is_file() else None


def extract_hicache_from_server_command(command: list[str] | str) -> dict[str, Any] | None:
    """Reconstruct a target contract directly from a declared server command."""

    flags = parse_server_command_tokens(command_tokens(command))
    if flags.get("enable_hierarchical_cache") != "true":
        return None
    page_size = parse_nonnegative_int_or_none(flags.get("page_size"))
    if not page_size:
        return None

    result: dict[str, Any] = {
        "enabled": True,
        "page_size": page_size,
        "write_policy": flags.get("hicache_write_policy", "write_through"),
        "prefetch_policy": flags.get("hicache_storage_prefetch_policy", "timeout"),
    }
    extra = parse_json_object_or_empty(flags.get("hicache_storage_backend_extra_config"))
    threshold_tokens = parse_nonnegative_int_or_none(extra.get("prefetch_threshold"))
    if threshold_tokens:
        result["prefetch_threshold_pages"] = (threshold_tokens + page_size - 1) // page_size

    timeout_fields = (
        ("prefetch_timeout_base", "prefetch_timeout_base_sec"),
        ("prefetch_timeout_per_ki_token", "prefetch_timeout_per_ki_token_sec"),
        ("prefetch_timeout_max", "prefetch_timeout_max_sec"),
    )
    timeout_values = [parse_nonnegative_float_or_none(extra.get(source)) for source, _ in timeout_fields]
    if all(value is not None for value in timeout_values):
        for (_, target), value in zip(timeout_fields, timeout_values):
            result[target] = value
    return apply_sglang_capacity_from_flags(flags, result)


def parse_json_object_or_empty(value: Any) -> dict[str, Any]:
    """Parse one command-line JSON object without accepting non-object values."""

    if not isinstance(value, str) or not value:
        return {}
    try:
        parsed = json.loads(value)
    except json.JSONDecodeError:
        return {}
    return parsed if isinstance(parsed, dict) else {}


def apply_sglang_capacity_from_server_cmd(run_dir: Path, hicache_config: dict[str, Any]) -> dict[str, Any]:
    """Resolve model capacities from the exact SGLang server command."""

    flags = parse_server_command_flags(run_dir / "server_cmd.txt")
    return apply_sglang_capacity_from_flags(flags, hicache_config)


def apply_sglang_capacity_from_flags(flags: dict[str, str], hicache_config: dict[str, Any]) -> dict[str, Any]:
    """Resolve cache capacities from already parsed server flags."""

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
    """Parse server arguments, retaining complete multi-value options."""

    if not path.is_file():
        return {}
    return parse_server_command_tokens(command_tokens(path.read_text(encoding="utf-8")))


def parse_server_command_tokens(tokens: list[str]) -> dict[str, str]:
    """Parse server options once, including supported model/TP aliases.

    Repeated aliases have the same last-option-wins behavior as argparse.
    Multi-value options such as NUMA placement retain every value.
    """

    aliases = {"model": "model_path", "tensor_parallel_size": "tp_size", "tp": "tp_size"}
    flags: dict[str, str] = {}
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if not token.startswith("--"):
            index += 1
            continue
        key, separator, first = token[2:].partition("=")
        values = [first] if separator else []
        index += 1
        while index < len(tokens) and not tokens[index].startswith("--"):
            values.append(tokens[index])
            index += 1
        key = key.replace("-", "_")
        flags[aliases.get(key, key)] = " ".join(values) if values else "true"
    return flags


def parse_nonnegative_int_or_none(value: Any) -> int | None:
    """Parse a non-negative integer, returning ``None`` when invalid."""

    try:
        if value is None:
            return None
        parsed = int(float(str(value)))
        return parsed if parsed >= 0 else None
    except (TypeError, ValueError):
        return None


def parse_nonnegative_float_or_none(value: Any) -> float | None:
    """Parse a non-negative float, returning ``None`` when invalid."""

    try:
        if value is None:
            return None
        parsed = float(str(value))
        return parsed if parsed >= 0.0 else None
    except (TypeError, ValueError):
        return None
