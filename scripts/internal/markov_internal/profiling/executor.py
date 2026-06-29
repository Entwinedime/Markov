"""单个 profiling run 执行器。"""

from __future__ import annotations

import subprocess
import sys
import time
from pathlib import Path
from typing import Any

from ..common.commands import command_from_config
from ..common.logging import log
from ..common.paths import ROOT_DIR
from ..common.process import start_process, stop_process, wait_for_ready
from .artifacts import write_profile_manifest, write_run_inputs
from .environments import build_bench_env, build_server_env
from .forced_workflow import preflight_forced_token_contract
from .profiler_api import (
    should_stop_torch_profiler_after_workload,
    start_torch_profiler,
    stop_torch_profiler,
    torch_profile_enabled,
)
from .runtime import (
    ModelConfigBackup,
    RunLayout,
    apply_model_config_overrides,
    build_bench_command,
    channel_config,
    expand_command_placeholders,
    restore_model_config,
)

sys.path.insert(0, str(ROOT_DIR / "src"))

from profiling import normalize_profiling_config  # noqa: E402


class ProfileRun:
    """单次 profiling 运行的执行器。"""

    def __init__(self, cfg: dict[str, Any], *, dry_run: bool) -> None:
        """规整单次 run 的配置、目录和 server/workload 命令。"""

        self.cfg = cfg
        self.dry_run = dry_run
        self.framework = str(cfg.get("framework", "sglang"))
        if self.framework != "sglang":
            raise ValueError("scripts/internal/entrypoints/profile.py currently supports framework=sglang")

        self.runtime = normalize_profiling_config(cfg)
        self.layout = RunLayout.from_config(cfg, framework=self.framework)
        self.server_cfg = cfg.get("server", {})
        self.server_command = expand_command_placeholders(command_from_config(self.server_cfg["command"]), self.layout, self.cfg)
        self.bench_command = build_bench_command(cfg.get("bench", {}), self.layout, self.cfg)

    def run(self) -> Path:
        """执行一次 profiling run，并在 finally 中写出 manifest。"""

        self.layout.prepare(clean=bool(self.cfg.get("clean_run_dir", False)))
        write_run_inputs(
            self.layout.run_dir,
            self.cfg,
            self.server_command,
            self.bench_command,
        )

        log(f"Run dir: {self.layout.run_dir}")
        started_at = time.time()
        if self.dry_run:
            write_profile_manifest(
                self.layout.run_dir,
                cfg=self.cfg,
                runtime=self.runtime,
                started_at=started_at,
                status="dry_run",
                dry_run=True,
            )
            return self.layout.run_dir

        status = "completed"
        error: str | None = None
        backup: ModelConfigBackup | None = None
        server_process: subprocess.Popen[Any] | None = None
        try:
            server_env = build_server_env(self.cfg, self.runtime, self.layout)
            backup = apply_model_config_overrides(self.cfg, self.server_command, self.layout)

            log("Starting SGLang server.")
            server_process = start_process(self.server_command, self.layout.log_dir / "server.log", server_env)
            self._wait_for_server(server_process)
            log("Server is ready.")

            profile_cfg = channel_config(self.cfg, "torch")
            torch_enabled = torch_profile_enabled(self.runtime, self.cfg)
            if torch_enabled:
                start_torch_profiler(self.layout, self.server_cfg, profile_cfg)

            if self.bench_command is not None:
                self._run_bench(server_env)

            if torch_enabled and should_stop_torch_profiler_after_workload(profile_cfg):
                stop_torch_profiler(self.layout, self.server_cfg, profile_cfg)

        except Exception as exc:
            status = "failed"
            error = str(exc)
            raise
        finally:
            stop_process(server_process)
            restore_model_config(backup)
            write_profile_manifest(
                self.layout.run_dir,
                cfg=self.cfg,
                runtime=self.runtime,
                started_at=started_at,
                status=status,
                dry_run=False,
                error=error,
            )

        if status == "failed" and error:
            raise RuntimeError(error)
        log("Profile run completed.")
        return self.layout.run_dir

    def _wait_for_server(self, process: subprocess.Popen[Any]) -> None:
        """等待 server ready，超时时暴露进程提前退出或健康检查失败。"""

        ready_url = self.server_cfg.get("ready_url", "http://127.0.0.1:30000/get_model_info")
        wait_for_ready(process, ready_url, int(self.server_cfg.get("ready_timeout_sec", 1800)))

    def _run_bench(self, server_env: dict[str, str]) -> None:
        """运行 workload，并移除只应注入 server 的采集环境。"""

        log("Running workload.")
        bench_env = build_bench_env(
            self.cfg,
            self.server_cfg,
            self.server_command,
            server_env,
            self.layout,
        )
        bench_proc = start_process(self.bench_command, self.layout.log_dir / "bench.log", bench_env)
        bench_code = bench_proc.wait()
        if bench_code != 0:
            raise RuntimeError(f"bench command failed, code={bench_code}")


def run_profile(cfg: dict[str, Any], dry_run: bool) -> Path:
    """执行单个已展开 profiling 配置。"""

    return ProfileRun(cfg, dry_run=dry_run).run()


def preflight_profile_config(cfg: dict[str, Any]) -> dict[str, Any]:
    """对单个已展开配置做只读/轻量 preflight。"""

    probe = ProfileRun(cfg, dry_run=True)
    metadata = cfg.get("metadata") if isinstance(cfg.get("metadata"), dict) else {}
    exp_id = str(cfg.get("id") or cfg.get("name") or "profile")
    return preflight_forced_token_contract(
        probe.bench_command,
        metadata,
        experiment_id=exp_id,
    )
