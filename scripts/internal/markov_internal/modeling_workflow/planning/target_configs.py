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
    raw = load_json(resolved)
    if not isinstance(raw, dict) or not isinstance(raw.get("hicache"), dict):
        raise ValueError(f"target config must contain a hicache object: {resolved}")
    fields: dict[str, Any] = dict(raw["hicache"])
    if any(field in fields for field in ("io_cost", "io_planning", "kv_bytes_per_page")):
        raise ValueError(f"target config cannot contain I/O model fields: {resolved}")
    if fields.get("page_size") is None:
        raise ValueError(f"target config requires hicache.page_size: {resolved}")
    label = str(raw.get("name") or resolved.stem)
    return TargetHiCacheConfig(label=label, fields=fields, source_path=resolved)


def load_target_configs(paths: list[Path]) -> tuple[TargetHiCacheConfig, ...]:
    """Load unique target requests in command-line order."""

    configs = tuple(load_target_config(path) for path in paths)
    labels = [config.label for config in configs]
    if len(labels) != len(set(labels)):
        raise ValueError("target config names must be unique within one prediction request")
    return configs
