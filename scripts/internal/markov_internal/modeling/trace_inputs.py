"""C++ modeling run 的 trace input 准备工具。"""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path
from typing import Any

from ..common.io import load_json
from ..common.paths import ROOT_DIR, map_repo_path, resolve_repo_path


def prepare_trace_inputs(
    config: dict[str, Any],
    input_cfg: dict[str, Any],
    manifest_path: Path | None,
    output_dir: Path,
) -> list[Path]:
    """根据 config 或 profile manifest 准备 C++ 后端输入 trace。"""

    if manifest_path is not None:
        merged_dir = output_dir / "merged_trace"
        reusable = load_reusable_merge_summary(merged_dir, manifest_path)
        if reusable:
            return reusable
        command = [
            sys.executable,
            str(ROOT_DIR / "scripts/trace/trace_merger.py"),
            "--manifest",
            str(manifest_path),
            "--out-dir",
            str(merged_dir),
        ]
        merge_cfg = config.get("trace_merge") if isinstance(config.get("trace_merge"), dict) else {}
        if "tolerance_us" in merge_cfg:
            command.extend(["--tolerance", str(merge_cfg["tolerance_us"])])
        if "search_window" in merge_cfg:
            command.extend(["--window", str(merge_cfg["search_window"])])
        if "margin_us" in merge_cfg:
            command.extend(["--margin", str(merge_cfg["margin_us"])])
        if "mode" in merge_cfg:
            command.extend(["--mode", str(merge_cfg["mode"])])
        subprocess.run(command, cwd=ROOT_DIR, check=True)
        summary = load_json(merged_dir / "merge_manifest_summary.json")
        return [required_repo_path(path) for path in summary.get("merged_trace_files", [])]

    raw_paths: list[Any] = []
    raw_paths.extend(input_cfg.get("trace_paths") or [])
    paths = [required_repo_path(str(path)) for path in raw_paths]
    existing = [path for path in paths if path.is_file()]
    if not existing:
        raise ValueError("modeling input has no trace files")
    return existing


def load_reusable_merge_summary(merged_dir: Path, manifest_path: Path) -> list[Path]:
    """复用同一输出目录中已完成、且 manifest 匹配的 trace merger 结果。"""

    summary_path = merged_dir / "merge_manifest_summary.json"
    if not summary_path.is_file():
        return []
    try:
        summary = load_json(summary_path)
    except json.JSONDecodeError:
        return []

    raw_manifest = summary.get("manifest_path")
    if not isinstance(raw_manifest, str):
        return []
    if map_repo_path(Path(raw_manifest)) != manifest_path:
        return []

    paths = [required_repo_path(str(path)) for path in summary.get("merged_trace_files", [])]
    if not paths or any(not path.is_file() for path in paths):
        return []
    return paths


def required_repo_path(value: Any) -> Path:
    """解析必填 repo path，缺失时抛出错误。"""

    path = resolve_repo_path(value)
    if path is None:
        raise ValueError("expected a non-empty path")
    return path
