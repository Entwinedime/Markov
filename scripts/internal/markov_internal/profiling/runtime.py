"""Runtime layout, placeholder, command, and temporary-config helpers."""

from __future__ import annotations

import json
import re
import shutil
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from ..common.commands import command_from_config, command_tokens
from ..common.io import load_json, write_json
from ..common.naming import sanitize
from ..common.paths import ROOT_DIR, resolve_repo_path


@dataclass(frozen=True)
class RunLayout:
    """Stable directory layout owned by one profiling run."""

    run_dir: Path
    log_dir: Path
    trace_dir: Path
    torch_trace_dir: Path
    ld_preload_trace_dir: Path
    bench_dir: Path

    @classmethod
    def from_config(cls, cfg: dict[str, Any], *, framework: str) -> RunLayout:
        """Derive a stable run layout from an expanded configuration."""

        name = sanitize(str(cfg.get("name", f"{framework}-profile")))
        run_root = resolve_repo_path(cfg.get("run_root")) or ROOT_DIR / "data/profile_runs" / framework
        run_id = cfg.get("run_id") or f"{time.strftime('%Y%m%d_%H%M%S')}_{name}"
        run_dir = run_root / sanitize(str(run_id))
        trace_dir = run_dir / "trace"
        return cls(
            run_dir=run_dir,
            log_dir=run_dir / "logs",
            trace_dir=trace_dir,
            torch_trace_dir=trace_dir / "torch",
            ld_preload_trace_dir=trace_dir / "ld_preload",
            bench_dir=run_dir / "bench",
        )

    def prepare(self, *, clean: bool) -> None:
        """Create required directories, optionally replacing the existing run."""

        if clean and self.run_dir.exists():
            shutil.rmtree(self.run_dir)
        for path in (
            self.log_dir,
            self.trace_dir,
            self.torch_trace_dir,
            self.ld_preload_trace_dir,
            self.bench_dir,
        ):
            path.mkdir(parents=True, exist_ok=True)


@dataclass(frozen=True)
class ModelConfigBackup:
    """Backup identity for a model config temporarily modified during capture."""

    config_path: Path
    backup_path: Path


def resolve_run_path(value: str | None, run_dir: Path) -> Path | None:
    """Resolve a path relative to the current run directory."""

    if not value:
        return None
    path = Path(value).expanduser()
    if path.is_absolute():
        return path
    return run_dir / path


CONFIG_PLACEHOLDER_ROOTS = {"metadata", "server", "bench", "env", "modeling"}


def config_placeholder_value(cfg: dict[str, Any], path: str) -> str | None:
    """Resolve a dotted config placeholder such as ``{metadata.foo}``."""

    parts = [part for part in path.split(".") if part]
    if len(parts) < 2 or parts[0] not in CONFIG_PLACEHOLDER_ROOTS:
        return None
    cursor: Any = cfg
    for part in parts:
        if not isinstance(cursor, dict) or part not in cursor:
            return None
        cursor = cursor[part]
    if isinstance(cursor, (dict, list)):
        return json.dumps(cursor, ensure_ascii=False, sort_keys=True)
    return str(cursor)


def expand_config_placeholders(value: str, cfg: dict[str, Any]) -> str:
    """Expand all dotted config placeholders in a string."""

    pattern = re.compile(r"\{([A-Za-z_][A-Za-z0-9_-]*(?:\.[A-Za-z0-9_-]+)+)\}")

    def replace(match: re.Match[str]) -> str:
        """Replace one regex-matched placeholder or reject an unknown path."""

        path = match.group(1)
        replacement = config_placeholder_value(cfg, path)
        if replacement is None:
            raise ValueError(f"unknown config placeholder: {{{path}}}")
        return replacement

    return pattern.sub(replace, value)


def expand_layout_placeholders(value: str, layout: RunLayout, cfg: dict[str, Any] | None = None) -> str:
    """Expand run-layout placeholders and optional dotted config values."""

    replacements = {
        "{run_dir}": str(layout.run_dir),
        "{trace_dir}": str(layout.trace_dir),
        "{bench_dir}": str(layout.bench_dir),
        "{log_dir}": str(layout.log_dir),
    }
    result = value
    for placeholder, replacement in replacements.items():
        result = result.replace(placeholder, replacement)
    if cfg is not None:
        result = expand_config_placeholders(result, cfg)
    return result


def expand_command_placeholders(
    command: list[str] | str,
    layout: RunLayout,
    cfg: dict[str, Any] | None = None,
) -> list[str] | str:
    """Expand placeholders while preserving a command's list/string form."""

    if isinstance(command, list):
        return [expand_layout_placeholders(item, layout, cfg) for item in command]
    return expand_layout_placeholders(command, layout, cfg)


def expand_runtime_value(value: Any, layout: RunLayout, cfg: dict[str, Any]) -> Any:
    """Recursively expand strings in list-valued runtime configuration."""

    if isinstance(value, str):
        return expand_layout_placeholders(value, layout, cfg)
    if isinstance(value, list):
        return [expand_runtime_value(item, layout, cfg) for item in value]
    return value


def append_cli_arg(command: list[str], key: str, value: Any) -> None:
    """Append one JSON config value using bench-serving CLI conventions."""

    option = "--" + key.replace("_", "-")
    if isinstance(value, bool):
        if value:
            command.append(option)
    elif isinstance(value, list):
        for item in value:
            command.extend([option, str(item)])
    elif value is not None:
        command.extend([option, str(value)])


def build_bench_command(
    bench: dict[str, Any], layout: RunLayout, cfg: dict[str, Any], *, framework: str
) -> list[str] | str | None:
    """Build an explicit or standard SGLang workload-driver command."""

    if not bench:
        return None
    if "command" in bench:
        return expand_command_placeholders(command_from_config(bench["command"]), layout, cfg)

    if framework != "sglang":
        raise ValueError(f"{framework} workloads require bench.command")
    kind = bench.get("kind", "sglang.bench_serving")
    if kind != "sglang.bench_serving":
        raise ValueError(f"unknown bench kind: {kind}")

    args = dict(bench.get("args", {}))
    args.setdefault("output_file", bench.get("output_file") or str(layout.bench_dir / "bench.jsonl"))

    command = ["python3", "-m", "sglang.bench_serving"]
    for key, value in args.items():
        append_cli_arg(command, key, expand_runtime_value(value, layout, cfg))
    return command


def parse_model_path(server_command: list[str] | str) -> str | None:
    """Extract the model path needed for temporary ``config.json`` overrides."""

    tokens = command_tokens(server_command)
    if not tokens:
        return None

    for index, token in enumerate(tokens):
        if token in {"--model-path", "--model_path"} and index + 1 < len(tokens):
            return tokens[index + 1]
        if token.startswith("--model-path="):
            return token.split("=", 1)[1]
        if token.startswith("--model_path="):
            return token.split("=", 1)[1]
    return None


def apply_model_config_overrides(
    cfg: dict[str, Any],
    server_command: list[str] | str,
    layout: RunLayout,
) -> ModelConfigBackup | None:
    """Apply model-config overrides after saving a run-local byte-for-byte backup."""

    overrides = cfg.get("model_config_overrides") or {}
    if not overrides:
        return None
    if not isinstance(overrides, dict):
        raise TypeError("model_config_overrides must be an object")

    model_path_value = (
        cfg.get("model_path") or cfg.get("server", {}).get("model_path") or parse_model_path(server_command)
    )
    model_path = resolve_repo_path(model_path_value) if model_path_value else None
    if model_path is None:
        raise ValueError("model_config_overrides requires model_path or --model-path")

    config_path = model_path / "config.json"
    if not config_path.is_file():
        raise FileNotFoundError(f"missing model config: {config_path}")

    backup_path = layout.run_dir / "_config_backup" / sanitize(model_path.name) / "config.json"
    backup_path.parent.mkdir(parents=True, exist_ok=True)
    backup_path.write_bytes(config_path.read_bytes())

    data = load_json(config_path)
    data.update(overrides)
    write_json(config_path, data)
    return ModelConfigBackup(config_path=config_path, backup_path=backup_path)


def restore_model_config(backup: ModelConfigBackup | None) -> None:
    """Restore a model config from the backup created before profiling."""

    if backup and backup.backup_path.is_file():
        backup.config_path.write_bytes(backup.backup_path.read_bytes())


def build_profile_body(profile: dict[str, Any], layout: RunLayout) -> dict[str, Any]:
    """Build the request body accepted by SGLang ``/start_profile``."""

    output_dir = resolve_run_path(profile.get("output_dir", "trace/torch"), layout.run_dir)
    body: dict[str, Any] = {"output_dir": str(output_dir)}
    for key in (
        "start_step",
        "num_steps",
        "activities",
        "profile_by_stage",
        "with_stack",
        "record_shapes",
        "merge_profiles",
        "profile_prefix",
        "profile_stages",
    ):
        if key in profile and profile[key] is not None:
            body[key] = profile[key]
    return body


def channel_config(cfg: dict[str, Any], profiling_key: str) -> dict[str, Any]:
    """Return one capture-channel object from the profiling config."""

    profiling = cfg.get("profiling") if isinstance(cfg.get("profiling"), dict) else {}
    current = profiling.get(profiling_key)
    if isinstance(current, dict):
        return current
    return {}
