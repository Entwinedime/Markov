"""项目内部脚本的路径约定。"""

from __future__ import annotations

import os
from pathlib import Path


CONTAINER_REPO_PREFIXES = ("/workspace/trace-sim", "/opt/trace-sim")
MODELING_CONTAINER_ENV = "TRACE_SIM_MODELING_CONTAINER"


def repo_root() -> Path:
    """从当前文件向上查找仓库根目录。"""

    current = Path(__file__).resolve()
    for parent in (current, *current.parents):
        if (parent / "pyproject.toml").is_file() and (parent / "scripts").is_dir():
            return parent
    raise RuntimeError(f"cannot locate repo root from {current}")


ROOT_DIR = repo_root()


def map_repo_path(path: Path) -> Path:
    """把容器内仓库前缀映射为当前 checkout 路径。"""

    raw = str(path)
    for prefix in CONTAINER_REPO_PREFIXES:
        if raw == prefix:
            return ROOT_DIR
        if raw.startswith(prefix + "/"):
            return ROOT_DIR / raw[len(prefix) + 1 :]
    return path


def resolve_repo_path(value: str | os.PathLike[str] | None) -> Path | None:
    """解析 repo-relative、host absolute 或 container absolute 路径。"""

    if value is None:
        return None
    path = Path(value).expanduser()
    if path.is_absolute():
        return map_repo_path(path)
    return ROOT_DIR / path


def running_in_modeling_container() -> bool:
    """判断当前进程是否在受支持的 modeling 容器内。"""

    root = str(ROOT_DIR)
    under_container_repo = any(root == prefix or root.startswith(prefix + "/") for prefix in CONTAINER_REPO_PREFIXES)
    has_container_marker = os.environ.get(MODELING_CONTAINER_ENV) == "1" or Path("/.dockerenv").exists()
    return under_container_repo and has_container_marker
