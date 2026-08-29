"""Lifecycle executor for one expanded profiling run."""

from __future__ import annotations

import shutil
import subprocess
import time
from pathlib import Path
from typing import Any

from ..common.commands import command_from_config
from ..common.logging import log
from ..common.paths import prepend_repo_src_to_sys_path
from ..common.process import start_process, stop_process, wait_for_ready
from .artifacts import write_profile_manifest, write_run_inputs
from .drain import capture_tail_policy
from .environments import build_bench_env, build_server_env
from .forced_workflow import preflight_forced_token_contract
from .frameworks import framework_adapter, validate_framework_channels
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
from .storage_cleanup import cleanup_run_local_hicache_storage

prepend_repo_src_to_sys_path()

from profiling import normalize_profiling_config  # noqa: E402


class ProfileRun:
    """Own the server, profiler, workload, restoration, and manifest lifecycle."""

    def __init__(self, cfg: dict[str, Any], *, dry_run: bool) -> None:
        """Normalize config, layout, and server/workload commands for one run."""

        self.cfg = cfg
        self.dry_run = dry_run
        self.framework = str(cfg.get("framework", "sglang"))
        self.adapter = framework_adapter(self.framework)
        self.runtime = normalize_profiling_config(cfg)
        validate_framework_channels(self.adapter, self.runtime.channels)
        # Validate capture-tail settings before starting a model server.
        capture_tail = capture_tail_policy(cfg)
        self.post_workload_drain_sec = capture_tail["post_workload_drain_sec"]
        self.cleanup_hicache_storage_after_run = bool(cfg.get("cleanup_hicache_storage_after_run", False))
        self.sync_filesystem_after_hicache_storage_cleanup = bool(
            cfg.get("sync_filesystem_after_hicache_storage_cleanup", False)
        )
        if self.sync_filesystem_after_hicache_storage_cleanup and not self.cleanup_hicache_storage_after_run:
            raise ValueError(
                "sync_filesystem_after_hicache_storage_cleanup requires cleanup_hicache_storage_after_run=true"
            )
        self.layout = RunLayout.from_config(cfg, framework=self.framework)
        self.server_cfg = cfg.get("server", {})
        self.server_command = expand_command_placeholders(
            command_from_config(self.server_cfg["command"]), self.layout, self.cfg
        )
        self.bench_command = build_bench_command(
            cfg.get("bench", {}), self.layout, self.cfg, framework=self.framework
        )

    def run(self) -> Path:
        """Execute the run and always persist a terminal profile manifest."""

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
            storage_cleanup = None
            if self.adapter.hicache:
                storage_cleanup = {
                    "status": (
                        "planned_after_server_exit"
                        if self.cleanup_hicache_storage_after_run
                        else "not_requested"
                    ),
                    "removed": False,
                    "filesystem_sync_after_removal": self.sync_filesystem_after_hicache_storage_cleanup,
                }
            write_profile_manifest(
                self.layout.run_dir,
                cfg=self.cfg,
                runtime=self.runtime,
                started_at=started_at,
                status="dry_run",
                dry_run=True,
                storage_cleanup=storage_cleanup,
            )
            return self.layout.run_dir

        status = "completed"
        error: str | None = None
        backup: ModelConfigBackup | None = None
        server_process: subprocess.Popen[Any] | None = None
        server_env: dict[str, str] = {}
        storage_cleanup: dict[str, Any] | None = None
        if self.adapter.hicache:
            storage_cleanup = {"status": "not_requested", "removed": False}
        try:
            server_env = build_server_env(self.cfg, self.runtime, self.layout, self.adapter)
            backup = apply_model_config_overrides(self.cfg, self.server_command, self.layout)

            server_process = self._start_server(server_env)
            log("Server is ready.")

            profile_cfg = channel_config(self.cfg, "torch")
            torch_enabled = self.adapter.profiler_api and torch_profile_enabled(self.runtime, self.cfg)
            if torch_enabled:
                start_torch_profiler(self.layout, self.server_cfg, profile_cfg)

            if self.bench_command is not None:
                self._run_bench(server_env)

            drain_sec = self.post_workload_drain_sec
            if drain_sec > 0:
                log(
                    f"Keeping capture channels active for {drain_sec:g}s after the workload "
                    "to retain asynchronous lifecycle tail evidence."
                )
                time.sleep(drain_sec)

            if torch_enabled and should_stop_torch_profiler_after_workload(profile_cfg):
                stop_torch_profiler(self.layout, self.server_cfg, profile_cfg)

        except Exception as exc:
            status = "failed"
            error = str(exc)
            raise
        finally:
            stop_process(server_process)
            shutdown_cooldown_sec = float(self.server_cfg.get("shutdown_cooldown_sec", 0))
            if server_process is not None and shutdown_cooldown_sec > 0:
                log(f"Cooling down after server shutdown for {shutdown_cooldown_sec:g}s.")
                time.sleep(shutdown_cooldown_sec)
            try:
                storage_cleanup = self._cleanup_hicache_storage(server_env)
            except Exception as cleanup_error:
                status = "failed"
                cleanup_message = f"HiCache storage cleanup failed: {cleanup_error}"
                error = f"{error}; {cleanup_message}" if error else cleanup_message
                storage_cleanup = {
                    "status": "failed",
                    "removed": False,
                    "error": str(cleanup_error),
                }
            restore_model_config(backup)
            write_profile_manifest(
                self.layout.run_dir,
                cfg=self.cfg,
                runtime=self.runtime,
                started_at=started_at,
                status=status,
                dry_run=False,
                error=error,
                storage_cleanup=storage_cleanup,
            )

        log("Profile run completed.")
        return self.layout.run_dir

    def _cleanup_hicache_storage(self, server_env: dict[str, str]) -> dict[str, Any] | None:
        """Remove run-local backend files after the server has exited.

        Trace, manifest, logs, and workload outputs are separate assets.  The
        storage directory contains only the file backend's temporary payloads;
        retaining it across a serial matrix both wastes disk and changes later
        cells' kernel page-cache pressure.
        """

        if not self.adapter.hicache:
            return None
        return cleanup_run_local_hicache_storage(
            self.layout.run_dir,
            server_env.get("SGLANG_HICACHE_FILE_BACKEND_STORAGE_DIR", ""),
            enabled=self.cleanup_hicache_storage_after_run,
            sync_after_removal=self.sync_filesystem_after_hicache_storage_cleanup,
        )

    def _wait_for_server(self, process: subprocess.Popen[Any]) -> None:
        """Wait for readiness while surfacing early exit and timeout failures."""

        ready_url = self.server_cfg.get("ready_url", self.adapter.default_ready_url)
        wait_for_ready(process, ready_url, int(self.server_cfg.get("ready_timeout_sec", 1800)))

    def _start_server(self, server_env: dict[str, str]) -> subprocess.Popen[Any]:
        """Start the server with bounded, clean retries before workload execution."""

        max_attempts = int(self.server_cfg.get("startup_max_attempts", 1))
        retry_delay_sec = float(self.server_cfg.get("startup_retry_delay_sec", 0))
        if max_attempts < 1:
            raise ValueError("server.startup_max_attempts must be at least 1")
        if retry_delay_sec < 0:
            raise ValueError("server.startup_retry_delay_sec must be non-negative")

        errors: list[str] = []
        for attempt in range(1, max_attempts + 1):
            log(f"Starting {self.framework} server (attempt {attempt}/{max_attempts}).")
            process = start_process(self.server_command, self.layout.log_dir / "server.log", server_env)
            try:
                self._wait_for_server(process)
                return process
            except Exception as error:
                errors.append(str(error))
                stop_process(process)
                if attempt >= max_attempts:
                    raise RuntimeError(
                        f"server failed to become ready after {max_attempts} attempts: {errors}"
                    ) from error
                self._reset_failed_startup_attempt(attempt, server_env)
                log(
                    f"Server startup attempt {attempt}/{max_attempts} failed: {error}; "
                    f"retrying after {retry_delay_sec:g}s."
                )
                time.sleep(retry_delay_sec)
        raise AssertionError("unreachable server startup retry state")

    def _reset_failed_startup_attempt(self, attempt: int, server_env: dict[str, str]) -> None:
        """Preserve the failed log and remove attempt-local runtime artifacts."""

        server_log = self.layout.log_dir / "server.log"
        if server_log.is_file():
            server_log.replace(self.layout.log_dir / f"server_startup_attempt_{attempt}.log")
        if self.layout.trace_dir.exists():
            shutil.rmtree(self.layout.trace_dir)
        storage_raw = server_env.get("SGLANG_HICACHE_FILE_BACKEND_STORAGE_DIR") if self.adapter.hicache else None
        if storage_raw:
            storage_dir = Path(storage_raw).resolve()
            if storage_dir.is_relative_to(self.layout.run_dir.resolve()) and storage_dir.exists():
                shutil.rmtree(storage_dir)
        self.layout.prepare(clean=False)

    def _run_bench(self, server_env: dict[str, str]) -> None:
        """Run the workload with server-only capture variables removed."""

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
    """Execute one already expanded profiling configuration."""

    return ProfileRun(cfg, dry_run=dry_run).run()


def preflight_profile_config(cfg: dict[str, Any]) -> dict[str, Any]:
    """Run read-only contract preflight for one expanded configuration."""

    probe = ProfileRun(cfg, dry_run=True)
    metadata = cfg.get("metadata") if isinstance(cfg.get("metadata"), dict) else {}
    exp_id = str(cfg.get("id") or cfg.get("name") or "profile")
    return preflight_forced_token_contract(
        probe.bench_command,
        metadata,
        experiment_id=exp_id,
    )
