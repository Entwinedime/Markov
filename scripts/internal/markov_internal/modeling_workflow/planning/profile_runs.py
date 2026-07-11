"""Profile discovery, selection, parsing, and prediction-matrix construction."""

from __future__ import annotations

import shlex
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from ...common.io import load_json
from ...common.manifest import existing_manifest_files
from ...common.paths import map_repo_path
from ..types import CacheStatePredictionRef, ProfileRunRef


@dataclass(frozen=True)
class ProfileRunDiscovery:
    """Discover profile runs from suite directories and explicit manifests."""

    profile_run_dirs: tuple[Path, ...]
    manifests: tuple[Path, ...]

    def discover(self) -> list[ProfileRunRef]:
        """Return parsed profile runs in deterministic input/config/run order."""

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
    """Convert one ``profile_manifest.json`` into the workflow run contract."""

    def from_manifest(self, manifest_path: Path) -> ProfileRunRef:
        """Parse a manifest and its referenced profiling configuration."""

        manifest = require_json_object(load_json(manifest_path), manifest_path)
        run_dir = map_repo_path(Path(str(manifest.get("run_dir") or manifest_path.parent)))
        config_path = map_repo_path(Path(str(manifest.get("config_path") or run_dir / "config.json")))
        config = require_json_object(load_json(config_path), config_path)
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
            python_probe_files=tuple(existing_manifest_files(sidecar.get("python_probe_files", []))),
            hicache_config=extract_hicache_modeling_config(config, run_dir),
        )


@dataclass(frozen=True)
class RunSelector:
    """Filter discovered runs using independent CLI membership selectors."""

    input_ids: frozenset[str]
    config_ids: frozenset[str]
    source_config_ids: frozenset[str]
    target_config_ids: frozenset[str]

    def filter(self, runs: list[ProfileRunRef]) -> list[ProfileRunRef]:
        """Return runs admitted by the requested input and config selectors."""

        config_filter = self.config_ids | self.source_config_ids | self.target_config_ids
        return [
            run
            for run in runs
            if (not self.input_ids or run.input_id in self.input_ids)
            and (not config_filter or run.config_id in config_filter)
        ]


@dataclass(frozen=True)
class PredictionMatrixBuilder:
    """Build source-to-target predictions within each workload input."""

    runs: list[ProfileRunRef]
    source_config_ids: frozenset[str]
    target_config_ids: frozenset[str]
    prediction_scope: frozenset[str]
    max_predictions: int = 0

    def build(self) -> list[CacheStatePredictionRef]:
        """Return a deterministic, optionally bounded prediction matrix."""

        selected: dict[tuple[str, str, str], CacheStatePredictionRef] = {}
        for by_config in group_runs_by_input(self.runs).values():
            sources = [
                run
                for run in by_config.values()
                if not self.source_config_ids or run.config_id in self.source_config_ids
            ]
            targets = [
                run
                for run in by_config.values()
                if not self.target_config_ids or run.config_id in self.target_config_ids
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
    """Return the first non-empty string candidate or ``unknown``."""

    for value in values:
        if isinstance(value, str) and value.strip():
            return value.strip()
    return "unknown"


def extract_hicache_modeling_config(config: dict[str, Any], run_dir: Path) -> dict[str, Any] | None:
    """Extract the target HiCache model configuration from a profile config."""

    modeling = config.get("modeling") if isinstance(config.get("modeling"), dict) else {}
    hicache = modeling.get("hicache") if isinstance(modeling.get("hicache"), dict) else {}
    if not hicache:
        return None
    return apply_sglang_capacity_from_server_cmd(run_dir, {"enabled": True, **hicache})


def apply_sglang_capacity_from_server_cmd(run_dir: Path, hicache_config: dict[str, Any]) -> dict[str, Any]:
    """Resolve model capacities from the exact SGLang server command."""

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
    """Parse ``--key value`` and ``--key=value`` server arguments."""

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


def group_runs_by_input(runs: list[ProfileRunRef]) -> dict[str, dict[str, ProfileRunRef]]:
    """Index one unambiguous profile run per input/config cell.

    A prediction matrix has no run-generation dimension. Silently choosing one
    of two manifests for the same input/config pair would make every downstream
    source/target cell depend on path ordering, so duplicate cells fail here.
    """

    grouped: dict[str, dict[str, ProfileRunRef]] = {}
    for run in runs:
        by_config = grouped.setdefault(run.input_id, {})
        existing = by_config.get(run.config_id)
        if existing is not None and existing.manifest_path != run.manifest_path:
            raise ValueError(
                f"Duplicate profile cell for input={run.input_id!r}, config={run.config_id!r}: "
                f"{existing.manifest_path} and {run.manifest_path}"
            )
        by_config[run.config_id] = run
    return {input_id: dict(sorted(by_config.items())) for input_id, by_config in sorted(grouped.items())}


def require_json_object(value: Any, path: Path) -> dict[str, Any]:
    """Require a top-level JSON object at a workflow contract boundary."""

    if not isinstance(value, dict):
        raise ValueError(f"Expected a JSON object in {path}")
    return value
