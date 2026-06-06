#!/usr/bin/env python3
"""LD_PRELOAD hook framework fixture。"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def build_ld_preload_hook() -> Path:
    """构建模板 hook so，验证 active 目录名和构建入口是 ld_preload。"""

    output = subprocess.check_output(
        ["bash", "scripts/internal/hooks/build.sh", "ld_preload"],
        cwd=ROOT,
        text=True,
    )
    path = Path(output.strip().splitlines()[-1])
    assert path.is_file(), path
    return path


def run_load_fixture(hook_path: Path) -> None:
    """模板 hook 没有硬编码业务 wrapper，只验证 LD_PRELOAD 加载不破坏进程。"""

    with tempfile.TemporaryDirectory() as raw_tmp:
        tmp = Path(raw_tmp)
        env = os.environ.copy()
        env["LD_PRELOAD"] = str(hook_path)
        env["HOOK_TRACE_OUTPUT"] = str(tmp / "ld_preload" / "cpu_trace.json")
        subprocess.check_call(
            [sys.executable, "-S", "-c", "print('ld_preload smoke')"],
            env=env,
            cwd=ROOT,
        )


def main() -> int:
    run_load_fixture(build_ld_preload_hook())
    print("ld_preload fixtures passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
