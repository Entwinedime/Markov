"""C++ modeling run 的 trace input 路径工具。"""

from __future__ import annotations

from pathlib import Path
from typing import Any

from ..common.io import load_json
from ..common.paths import map_repo_path


def prepare_trace_inputs(
    config: dict[str, Any],
    _input_cfg: dict[str, Any],
    manifest_path: Path | None,
    _output_dir: Path,
) -> list[Path]:
    """根据 config 或 profile manifest 返回 validation 可记录的原始 trace 路径。

    当前 C++ 后端在 manifest 模式下直接读取 profile manifest，不再由 Python
    物化 merged trace。这个函数只保留显式 trace_paths 入口和 validation
    artifact 中需要展示的原始 trace 文件列表。
    """

    if manifest_path is not None:
        channels = trace_channels_from_config(config)
        return trace_paths_from_manifest(manifest_path, channels=channels)

    raise ValueError("modeling input requires profile manifest")


def trace_channels_from_config(config: dict[str, Any]) -> set[str] | None:
    """返回 C++ 实际消费的 trace channel 白名单；None 表示全量。"""

    cpp_cfg = config.get("cpp_trace_graph") if isinstance(config.get("cpp_trace_graph"), dict) else {}
    raw = cpp_cfg.get("trace_channels", config.get("trace_channels"))
    if raw is None:
        return None
    if isinstance(raw, str):
        items = raw.split(",")
    elif isinstance(raw, list):
        items = raw
    else:
        return None
    channels: set[str] = set()
    aliases = {
        "torch": "torch",
        "ld": "ld_preload",
        "ld-preload": "ld_preload",
        "ld_preload": "ld_preload",
        "probe": "python_probe",
        "python-probe": "python_probe",
        "python_probe": "python_probe",
    }
    for item in items:
        token = str(item).strip().lower()
        if not token:
            continue
        if token == "all":
            return None
        if token in aliases:
            channels.add(aliases[token])
    return channels or None


def trace_paths_from_manifest(manifest_path: Path, *, channels: set[str] | None = None) -> list[Path]:
    """展开 profile manifest 中存在的原始 trace 文件。"""

    manifest = load_json(manifest_path)
    trace = manifest.get("trace") if isinstance(manifest.get("trace"), dict) else {}
    sidecar = manifest.get("sidecar") if isinstance(manifest.get("sidecar"), dict) else {}
    paths: list[Path] = []
    if channels is None or "torch" in channels:
        paths.extend(existing_manifest_paths(trace.get("torch_trace_files", [])))
    if channels is None or "ld_preload" in channels:
        paths.extend(existing_manifest_paths(trace.get("ld_preload_trace_files", [])))
    if channels is None or "python_probe" in channels:
        paths.extend(existing_manifest_paths(sidecar.get("python_probe_files", [])))
    return sorted(dict.fromkeys(paths))


def existing_manifest_paths(entries: Any) -> list[Path]:
    """从 manifest path 条目中筛出真实存在的文件。"""

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
    return paths
