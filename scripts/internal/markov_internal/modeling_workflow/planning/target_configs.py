"""Loading of explicit target HiCache policy/capacity inputs."""

from __future__ import annotations

from pathlib import Path
from typing import Any

from ...common.io import load_json
from ...common.paths import require_repo_path
from ..types import TargetHiCacheConfig


def load_target_config(path: Path) -> TargetHiCacheConfig:
    """Load ``{name?, hicache}`` without accepting observation or cost fields."""

    resolved = require_repo_path(path)
    return parse_target_config(load_json(resolved), resolved)


def parse_target_config(raw: dict[str, Any], source_path: Path | None = None) -> TargetHiCacheConfig:
    """Use one policy-only boundary for explicit files and group declarations."""

    if not isinstance(raw, dict) or not isinstance(raw.get("hicache"), dict):
        raise ValueError(f"target config must contain a hicache object: {source_path}")
    if set(raw) - {"name", "hicache"}:
        raise ValueError("target declaration accepts only name and hicache policy/capacity")
    fields: dict[str, Any] = dict(raw["hicache"])
    if any(field in fields for field in ("io_cost", "phase_cost", "kv_bytes_per_page", "dag_patch")):
        raise ValueError(f"target config cannot contain HiCache model fields: {source_path}")
    if fields.get("page_size") is None:
        raise ValueError(f"target config requires hicache.page_size: {source_path}")
    label = str(raw.get("name") or (source_path.stem if source_path else ""))
    if not label:
        raise ValueError("inline target declaration requires a name")
    return TargetHiCacheConfig(label=label, fields=fields, source_path=source_path)


def load_target_configs(paths: list[Path]) -> tuple[TargetHiCacheConfig, ...]:
    """Load unique target requests in command-line order."""

    configs = tuple(load_target_config(path) for path in paths)
    labels = [config.label for config in configs]
    if len(labels) != len(set(labels)):
        raise ValueError("target config names must be unique within one prediction request")
    return configs
