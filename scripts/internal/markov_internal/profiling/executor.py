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
from .environments import build_bench_env, build_server_env
from .forced_workflow import preflight_forced_token_contract
from .frameworks import framework_adapter, validate_framework_channels
from .probe_targets import select_python_probe_targets
from .profiler_api import (
    should_stop_torch_profiler_after_workload,
    start_torch_profiler,
    stop_torch_profiler,
)
from .runtime import (
    RunLayout,
    build_bench_command,
    expand_runtime_value,
    model_path_from_config,
    temporary_model_config,
)
from .storage_cleanup import cleanup_run_local_hicache_storage

prepend_repo_src_to_sys_path()

from profiling import normalize_profiling_config  # noqa: E402


class ProfileRun:
    """Own the server, profiler, workload, restoration, and manifest lifecycle."""

    def __init__(self, cfg: dict[str, Any], *, dry_run: bool) -> None:
        """Prepare and check one run without starting processes or writing files."""

        self.cfg = cfg
        self.dry_run = dry_run
        self.framework = str(cfg.get("framework", "sglang"))
        self.adapter = framework_adapter(self.framework)
        self.runtime = normalize_profiling_config(cfg)
        validate_framework_channels(self.adapter, self.runtime.channels)
        self.python_targets = (
            select_python_probe_targets(self.runtime.python_consumers, diagnostics=self.runtime.python_diagnostics)
            if self.runtime.enabled and "python_probe" in self.runtime.channels
            else []
        )

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
        self.server_command = expand_runtime_value(
            command_from_config(self.server_cfg["command"]), self.layout, self.cfg
        )
        self.bench_command = build_bench_command(cfg.get("bench", {}), self.layout, self.cfg, framework=self.framework)
        self.model_path = model_path_from_config(cfg, self.server_command)

        preflight_forced_token_contract(
            self.bench_command,
            experiment_id=str(self.cfg.get("id") or self.cfg.get("name") or "profile"),
        )

    def run(self) -> Path:
        """Execute the run and always persist a terminal profile manifest."""

        clean = bool(self.cfg.get("clean_run_dir", False))
        if self.layout.run_dir.exists() and any(self.layout.run_dir.iterdir()) and (self.dry_run or not clean):
            raise FileExistsError(f"Profiling run directory is not empty: {self.layout.run_dir}; choose a new run_id")
        self.layout.prepare(clean=clean and not self.dry_run)
        write_run_inputs(
            self.layout.run_dir,
            self.cfg,
            self.server_command,
            self.bench_command,
        )

        log(f"Run dir: {self.layout.run_dir}")
        started_at = time.time()
        status = "dry_run" if self.dry_run else "completed"
        error: str | None = None
        server_process: subprocess.Popen[Any] | None = None
        server_env: dict[str, str] = {}
        workload_started = False
        storage_cleanup: dict[str, Any] | None = None
        if self.adapter.hicache:
            storage_cleanup = {"status": "not_requested", "removed": False}
            if self.dry_run:
                if self.cleanup_hicache_storage_after_run:
                    storage_cleanup["status"] = "planned_after_server_exit"
                storage_cleanup["filesystem_sync_after_removal"] = self.sync_filesystem_after_hicache_storage_cleanup

        try:
            if self.dry_run:
                return self.layout.run_dir

            server_env = build_server_env(self.cfg, self.runtime, self.layout, self.adapter, self.python_targets)
            with temporary_model_config(self.cfg, self.model_path, self.layout):
                try:
                    server_process = self._start_server(server_env)
                    log("Server is ready.")

                    profile_cfg = self.runtime.channel_options["torch"]
                    torch_enabled = (
                        self.adapter.profiler_api
                        and self.runtime.enabled
                        and "torch" in self.runtime.channels
                        and profile_cfg.get("enabled", True)
                    )
                    if torch_enabled:
                        start_torch_profiler(self.layout, self.server_cfg, self.runtime)

                    if self.bench_command is not None:
                        workload_started = True
                        self._run_bench(server_env)

                    drain_sec = self.runtime.post_workload_drain_sec
                    if drain_sec > 0:
                        log(
                            f"Keeping capture channels active for {drain_sec:g}s after the workload "
                            "to retain asynchronous lifecycle tail evidence."
                        )
                        time.sleep(drain_sec)

                    if torch_enabled and should_stop_torch_profiler_after_workload(profile_cfg):
                        stop_torch_profiler(self.layout, self.server_cfg, profile_cfg)
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
        except BaseException as exc:
            status = "failed"
            error = str(exc) or type(exc).__name__
            raise
        finally:
            write_profile_manifest(
                self.layout.run_dir,
                cfg=self.cfg,
                runtime=self.runtime,
                python_targets=self.python_targets,
                started_at=started_at,
                status=status,
                dry_run=self.dry_run,
                workload_started=workload_started,
                error=error,
                storage_cleanup=storage_cleanup,
            )

        if status == "failed":
            raise RuntimeError(error)
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
                wait_for_ready(
                    process,
                    self.server_cfg.get("ready_url", self.adapter.default_ready_url),
                    int(self.server_cfg.get("ready_timeout_sec", 1800)),
                )
                return process
            except BaseException as error:
                stop_process(process)
                # Interrupts release the process but must not start a retry.
                if not isinstance(error, Exception):
                    raise
                errors.append(str(error))
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
            cleanup_run_local_hicache_storage(self.layout.run_dir, storage_raw, enabled=True)
        self.layout.prepare(clean=False)

    def _run_bench(self, server_env: dict[str, str]) -> None:
        """Run the workload with server-only capture variables removed."""

        log("Running workload.")
        bench_env = build_bench_env(
            self.cfg,
            server_env,
            self.layout,
            self.model_path,
        )
        bench_proc = start_process(self.bench_command, self.layout.log_dir / "bench.log", bench_env)
        try:
            bench_code = bench_proc.wait()
        finally:
            stop_process(bench_proc)
        if bench_code != 0:
            raise RuntimeError(f"bench command failed, code={bench_code}")
