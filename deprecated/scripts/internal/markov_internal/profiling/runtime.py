"""Runtime layout, placeholder, command, and temporary-config helpers."""

from __future__ import annotations

import json
import re
import shutil
import time
from collections.abc import Iterator
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from ..common.commands import command_from_config, command_tokens
from ..common.io import write_json
from ..common.naming import sanitize
from ..common.paths import ROOT_DIR, resolve_repo_path


@dataclass(frozen=True)
class RunLayout:
    """Stable directory layout owned by one profiling run."""

    run_dir: Path
    log_dir: Path
    trace_dir: Path
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
            bench_dir=run_dir / "bench",
        )

    def prepare(self, *, clean: bool) -> None:
        """Create required directories, optionally replacing the existing run."""

        if clean and self.run_dir.exists():
            shutil.rmtree(self.run_dir)
        for path in (
            self.log_dir,
            self.trace_dir / "torch",
            self.trace_dir / "ld_preload",
            self.bench_dir,
        ):
            path.mkdir(parents=True, exist_ok=True)


CONFIG_PLACEHOLDER_ROOTS = {"metadata", "server", "bench", "env", "modeling"}


def expand_layout_placeholders(value: str, layout: RunLayout, cfg: dict[str, Any]) -> str:
    """Expand run paths, then declared config values, for commands and environments."""

    replacements = {
        "{run_dir}": str(layout.run_dir),
        "{trace_dir}": str(layout.trace_dir),
        "{bench_dir}": str(layout.bench_dir),
        "{log_dir}": str(layout.log_dir),
    }
    result = value
    for placeholder, replacement in replacements.items():
        result = result.replace(placeholder, replacement)

    def replace(match: re.Match[str]) -> str:
        path = match.group(1)
        parts = path.split(".")
        if parts[0] not in CONFIG_PLACEHOLDER_ROOTS:
            raise ValueError(f"unknown config placeholder: {{{path}}}")

        cursor: Any = cfg
        for part in parts:
            if not isinstance(cursor, dict) or part not in cursor:
                raise ValueError(f"unknown config placeholder: {{{path}}}")
            cursor = cursor[part]
        return (
            json.dumps(cursor, ensure_ascii=False, sort_keys=True) if isinstance(cursor, (dict, list)) else str(cursor)
        )

    return re.sub(r"\{([A-Za-z_][A-Za-z0-9_-]*(?:\.[A-Za-z0-9_-]+)+)\}", replace, result)


def expand_runtime_value(value: Any, layout: RunLayout, cfg: dict[str, Any]) -> Any:
    """Expand command/argument strings and lists, retaining other config values."""

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
        return expand_runtime_value(command_from_config(bench["command"]), layout, cfg)

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


def model_path_from_config(cfg: dict[str, Any], server_command: list[str] | str) -> str | None:
    """Select the explicit model reference, otherwise the last command-line value.

    Retain model IDs and relative paths verbatim; only local config overrides
    resolve the reference to a filesystem path.
    """

    value = cfg.get("model_path") or cfg.get("server", {}).get("model_path")
    if value:
        return value

    tokens = command_tokens(server_command)
    for index in range(len(tokens) - 1, -1, -1):
        key, separator, value = tokens[index].partition("=")
        if key in {"--model-path", "--model_path"}:
            return value if separator else tokens[index + 1]
    return None


@contextmanager
def temporary_model_config(
    cfg: dict[str, Any],
    model_path_value: str | None,
    layout: RunLayout,
) -> Iterator[None]:
    """Restore the original bytes after capture, including failed override writes."""

    overrides = cfg.get("model_config_overrides") or {}
    if not overrides:
        yield
        return
    if not isinstance(overrides, dict):
        raise TypeError("model_config_overrides must be an object")

    model_path = resolve_repo_path(model_path_value) if model_path_value else None
    if model_path is None:
        raise ValueError("model_config_overrides requires model_path or --model-path")

    config_path = model_path / "config.json"
    backup_path = layout.run_dir / "_config_backup" / sanitize(model_path.name) / "config.json"
    backup_path.parent.mkdir(parents=True, exist_ok=True)
    original = config_path.read_bytes()
    backup_path.write_bytes(original)
    data = json.loads(original)
    data.update(overrides)
    try:
        write_json(config_path, data)
        yield
    finally:
        config_path.write_bytes(original)
