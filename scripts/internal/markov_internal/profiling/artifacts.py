"""profiling run artifact 写出工具。"""

from __future__ import annotations

import sys
import time
from pathlib import Path
from typing import Any

from ..common.commands import command_to_text
from ..common.io import write_json
from ..common.paths import ROOT_DIR

sys.path.insert(0, str(ROOT_DIR / "src"))

from profiling import build_profile_manifest  # noqa: E402


def write_run_inputs(
    run_dir: Path,
    cfg: dict[str, Any],
    server_command: list[str] | str,
    bench_command: list[str] | str | None,
) -> None:
    """保存本次运行的原始配置和展开后的命令，供复现使用。"""

    write_json(run_dir / "config.json", cfg)
    (run_dir / "server_cmd.txt").write_text(
        command_to_text(server_command) + "\n",
        encoding="utf-8",
    )
    if bench_command is not None:
        (run_dir / "bench_cmd.txt").write_text(
            command_to_text(bench_command) + "\n",
            encoding="utf-8",
        )


def write_profile_manifest(
    run_dir: Path,
    cfg: dict[str, Any],
    runtime: Any,
    *,
    started_at: float,
    status: str,
    dry_run: bool,
    error: str | None = None,
) -> None:
    """写出 profile manifest，作为后续 merge/modeling 的入口。"""

    manifest = build_profile_manifest(
        run_dir=run_dir,
        cfg=cfg,
        runtime=runtime,
        started_at=started_at,
        ended_at=time.time(),
        status=status,
        dry_run=dry_run,
        error=error,
    )
    write_json(run_dir / "profile_manifest.json", manifest)
