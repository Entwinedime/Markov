#!/usr/bin/env python3
"""SGLang profiling runner。

本脚本只负责启动被测进程、注入采集环境、运行 workload，并写出 profile manifest。
建模判断不放在这里，避免 profiling 阶段和 modeling 阶段互相污染。
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Any


ROOT_DIR = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT_DIR / "src"))

from profiling import build_profile_manifest, normalize_profiling_config  # noqa: E402
from profiling.config import ProfilingRuntimeConfig  # noqa: E402


INTERNAL_SUITE_KEYS = {"experiments", "continue_on_error", "$unset"}
PYTHON_PROBE_ROOT = ROOT_DIR / "src/profiling/python_probe"
BENCH_ENV_REMOVE_KEYS = (
    "LD_PRELOAD",
    "HOOK_TRACE_OUTPUT",
    "TRACE_SIM_PYTHON_PROBE",
    "TRACE_SIM_PYTHON_PROBES",
    "TRACE_SIM_PYTHON_PROBE_TARGETS",
    "TRACE_SIM_PYTHON_PROBE_OUTPUT",
    "TRACE_SIM_PYTHON_PROBE_DEBUG",
    "TRACE_SIM_HICACHE_STATE_TRACE",
)


@dataclass(frozen=True)
class RunLayout:
    """一次 profiling 运行的目录布局。

    runner 所有路径先在这里规整，后续执行逻辑只使用绝对路径，避免路径相对
    run dir 或 repo root 的语义混在一起。
    """

    run_dir: Path
    log_dir: Path
    trace_dir: Path
    torch_trace_dir: Path
    ld_preload_trace_dir: Path
    bench_dir: Path

    @classmethod
    def from_config(cls, cfg: dict[str, Any], *, framework: str) -> "RunLayout":
        name = sanitize(str(cfg.get("name", f"{framework}-profile")))
        run_root = resolve_repo_path(cfg.get("run_root")) or ROOT_DIR / "data/profile_runs" / framework
        run_id = cfg.get("run_id") or f"{time.strftime('%Y%m%d_%H%M%S')}_{name}"
        run_dir = run_root / sanitize(str(run_id))
        trace_dir = run_dir / "trace"
        return cls(
            run_dir=run_dir,
            log_dir=run_dir / "logs",
            trace_dir=trace_dir,
            torch_trace_dir=trace_dir / "torch",
            ld_preload_trace_dir=trace_dir / "ld_preload",
            bench_dir=run_dir / "bench",
        )

    def prepare(self, *, clean: bool) -> None:
        if clean and self.run_dir.exists():
            shutil.rmtree(self.run_dir)
        for path in (
            self.log_dir,
            self.trace_dir,
            self.torch_trace_dir,
            self.ld_preload_trace_dir,
            self.bench_dir,
        ):
            path.mkdir(parents=True, exist_ok=True)


@dataclass(frozen=True)
class ModelConfigBackup:
    """被临时修改的模型 config 备份信息。"""

    config_path: Path
    backup_path: Path


def log(message: str) -> None:
    timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
    print(f"[{timestamp}] {message}", flush=True)


def sanitize(value: str) -> str:
    value = re.sub(r"[^A-Za-z0-9_.-]+", "_", value.strip())
    return value.strip("._-") or "profile"


def load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as file_obj:
        return json.load(file_obj)


def dump_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as file_obj:
        json.dump(value, file_obj, indent=2, ensure_ascii=False)
        file_obj.write("\n")


def resolve_repo_path(value: str | None) -> Path | None:
    if not value:
        return None
    path = Path(value).expanduser()
    if path.is_absolute():
        return path
    return ROOT_DIR / path


def resolve_run_path(value: str | None, run_dir: Path) -> Path | None:
    if not value:
        return None
    path = Path(value).expanduser()
    if path.is_absolute():
        return path
    return run_dir / path


def deep_merge(base: dict[str, Any], override: dict[str, Any]) -> dict[str, Any]:
    """递归合并 suite common 配置和单个 experiment 配置。"""

    merged = dict(base)
    for key, value in override.items():
        if key == "$unset":
            continue
        if key in merged and isinstance(merged[key], dict) and isinstance(value, dict):
            merged[key] = deep_merge(merged[key], value)
        else:
            merged[key] = value
    return merged


def delete_path(value: dict[str, Any], path: str) -> None:
    """按点分路径删除字段，用于 suite experiment 覆盖 common 配置。"""

    parts = [part for part in path.split(".") if part]
    if not parts:
        raise ValueError("$unset entries must not be empty")

    cursor: Any = value
    for part in parts[:-1]:
        if not isinstance(cursor, dict) or part not in cursor:
            return
        cursor = cursor[part]
    if isinstance(cursor, dict):
        cursor.pop(parts[-1], None)


def apply_unset(value: dict[str, Any], paths: Any) -> None:
    if paths is None:
        return
    if not isinstance(paths, list) or not all(isinstance(path, str) for path in paths):
        raise TypeError("$unset must be a list of dot-separated paths")
    for path in paths:
        delete_path(value, path)


def command_from_config(command: Any) -> list[str] | str:
    if isinstance(command, list) and all(isinstance(item, str) for item in command):
        return command
    if isinstance(command, str):
        return command
    raise TypeError("command must be either a string or a list of strings")


def command_to_text(command: list[str] | str) -> str:
    if isinstance(command, list):
        return shlex.join(command)
    return command


def expand_command_placeholders(command: list[str] | str, layout: RunLayout) -> list[str] | str:
    """替换显式命令中的运行目录占位符。

    这只影响 runner 启动 server / bench 的命令字符串，方便实验配置把 workload
    结果写入本次 run dir，而不是写死到全局目录。
    """

    replacements = {
        "{run_dir}": str(layout.run_dir),
        "{trace_dir}": str(layout.trace_dir),
        "{bench_dir}": str(layout.bench_dir),
        "{log_dir}": str(layout.log_dir),
    }

    def expand(value: str) -> str:
        for placeholder, replacement in replacements.items():
            value = value.replace(placeholder, replacement)
        return value

    if isinstance(command, list):
        return [expand(item) for item in command]
    return expand(command)


def expand_layout_placeholders(value: str, layout: RunLayout) -> str:
    """替换配置字符串中的运行目录占位符。

    server / bench 命令和 env 都使用同一套占位符，避免实验配置把
    HiCache storage 或 trace 输出写死到全局目录，污染跨配置验证。
    """

    replacements = {
        "{run_dir}": str(layout.run_dir),
        "{trace_dir}": str(layout.trace_dir),
        "{bench_dir}": str(layout.bench_dir),
        "{log_dir}": str(layout.log_dir),
    }
    result = value
    for placeholder, replacement in replacements.items():
        result = result.replace(placeholder, replacement)
    return result


def append_cli_arg(command: list[str], key: str, value: Any) -> None:
    option = "--" + key.replace("_", "-")
    if isinstance(value, bool):
        if value:
            command.append(option)
    elif isinstance(value, list):
        for item in value:
            command.extend([option, str(item)])
    elif value is not None:
        command.extend([option, str(value)])


def build_bench_command(bench: dict[str, Any], layout: RunLayout) -> list[str] | str | None:
    """从配置构造 workload driver 命令。

    显式 `bench.command` 优先；否则按 SGLang bench_serving 的参数对象生成命令。
    """

    if not bench:
        return None
    if "command" in bench:
        return expand_command_placeholders(command_from_config(bench["command"]), layout)

    kind = bench.get("kind", "sglang.bench_serving")
    if kind != "sglang.bench_serving":
        raise ValueError(f"unknown bench kind: {kind}")

    args = dict(bench.get("args", {}))
    args.setdefault("output_file", bench.get("output_file") or str(layout.bench_dir / "bench.jsonl"))

    command = ["python3", "-m", "sglang.bench_serving"]
    for key, value in args.items():
        append_cli_arg(command, key, value)
    return command


def parse_model_path(server_command: list[str] | str) -> str | None:
    """从 server 命令中解析模型路径，用于临时覆盖 config.json。"""

    if isinstance(server_command, str):
        try:
            tokens = shlex.split(server_command)
        except ValueError:
            return None
    else:
        tokens = server_command

    for index, token in enumerate(tokens):
        if token in {"--model-path", "--model_path"} and index + 1 < len(tokens):
            return tokens[index + 1]
        if token.startswith("--model-path="):
            return token.split("=", 1)[1]
        if token.startswith("--model_path="):
            return token.split("=", 1)[1]
    return None


def apply_model_config_overrides(
    cfg: dict[str, Any],
    server_command: list[str] | str,
    layout: RunLayout,
) -> ModelConfigBackup | None:
    """临时修改模型 config.json，并保留备份以便 finally 恢复。"""

    overrides = cfg.get("model_config_overrides") or {}
    if not overrides:
        return None
    if not isinstance(overrides, dict):
        raise TypeError("model_config_overrides must be an object")

    model_path_value = (
        cfg.get("model_path")
        or cfg.get("server", {}).get("model_path")
        or parse_model_path(server_command)
    )
    model_path = resolve_repo_path(model_path_value) if model_path_value else None
    if model_path is None:
        raise ValueError("model_config_overrides requires model_path or --model-path")

    config_path = model_path / "config.json"
    if not config_path.is_file():
        raise FileNotFoundError(f"missing model config: {config_path}")

    backup_path = layout.run_dir / "_config_backup" / sanitize(model_path.name) / "config.json"
    backup_path.parent.mkdir(parents=True, exist_ok=True)
    backup_path.write_bytes(config_path.read_bytes())

    data = load_json(config_path)
    data.update(overrides)
    dump_json(config_path, data)
    return ModelConfigBackup(config_path=config_path, backup_path=backup_path)


def restore_model_config(backup: ModelConfigBackup | None) -> None:
    if backup and backup.backup_path.is_file():
        backup.config_path.write_bytes(backup.backup_path.read_bytes())


def prepend_pythonpath(env: dict[str, str], path: Path) -> None:
    """把 probe 路径放到 PYTHONPATH 前面，确保 sitecustomize 可以被 Python 发现。"""

    current = env.get("PYTHONPATH")
    env["PYTHONPATH"] = str(path) + (os.pathsep + current if current else "")


def remove_pythonpath_entry(env: dict[str, str], path: Path) -> None:
    """从 PYTHONPATH 移除 runner 注入项，避免 bench client 被误插桩。"""

    current = env.get("PYTHONPATH")
    if not current:
        return
    filtered = [item for item in current.split(os.pathsep) if item != str(path)]
    if filtered:
        env["PYTHONPATH"] = os.pathsep.join(filtered)
    else:
        env.pop("PYTHONPATH", None)


def api_base_from_ready_url(ready_url: str) -> str:
    parsed = urllib.parse.urlsplit(ready_url)
    if not parsed.scheme or not parsed.netloc:
        raise ValueError(f"ready_url must be absolute: {ready_url}")
    return urllib.parse.urlunsplit((parsed.scheme, parsed.netloc, "", "", ""))


def post_json(url: str, body: dict[str, Any] | None, timeout: int = 60) -> Any:
    data = None
    headers = {}
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"
    request = urllib.request.Request(url, data=data, headers=headers, method="POST")
    with urllib.request.urlopen(request, timeout=timeout) as response:
        payload = response.read()
    if not payload:
        return None
    try:
        return json.loads(payload.decode("utf-8"))
    except json.JSONDecodeError:
        return payload.decode("utf-8", errors="replace")


def wait_for_ready(process: subprocess.Popen[Any], ready_url: str, timeout_sec: int) -> None:
    """等待 server ready，同时监控进程是否提前退出。"""

    start = time.monotonic()
    while True:
        if process.poll() is not None:
            raise RuntimeError(f"server exited before ready, code={process.returncode}")
        try:
            with urllib.request.urlopen(ready_url, timeout=5) as response:
                if 200 <= response.status < 500:
                    return
        except (urllib.error.URLError, TimeoutError):
            pass

        if time.monotonic() - start > timeout_sec:
            raise TimeoutError(f"server did not become ready within {timeout_sec}s")
        time.sleep(5)


def start_process(
    command: list[str] | str,
    log_path: Path,
    env: dict[str, str],
) -> subprocess.Popen[Any]:
    """启动子进程并把 stdout/stderr 写入日志文件。"""

    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_file = log_path.open("wb")
    try:
        return subprocess.Popen(
            command,
            cwd=ROOT_DIR,
            env=env,
            stdout=log_file,
            stderr=subprocess.STDOUT,
            shell=isinstance(command, str),
            preexec_fn=os.setsid,
        )
    finally:
        # Popen 已经复制 fd，父进程这里可以关闭，避免长时间 suite 泄漏文件句柄。
        log_file.close()


def stop_process(process: subprocess.Popen[Any] | None, timeout_sec: int = 20) -> None:
    if process is None or process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=timeout_sec)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait()
    except ProcessLookupError:
        pass


def build_profile_body(profile: dict[str, Any], layout: RunLayout) -> dict[str, Any]:
    output_dir = resolve_run_path(profile.get("output_dir", "trace/torch"), layout.run_dir)
    body: dict[str, Any] = {"output_dir": str(output_dir)}
    for key in (
        "start_step",
        "num_steps",
        "activities",
        "profile_by_stage",
        "with_stack",
        "record_shapes",
        "merge_profiles",
        "profile_prefix",
        "profile_stages",
    ):
        if key in profile and profile[key] is not None:
            body[key] = profile[key]
    return body


def channel_config(cfg: dict[str, Any], profiling_key: str) -> dict[str, Any]:
    """读取采集渠道配置。

    主线 schema 只使用 `profiling.<channel>`，不读取旧顶层兼容字段。
    """

    profiling = cfg.get("profiling") if isinstance(cfg.get("profiling"), dict) else {}
    current = profiling.get(profiling_key)
    if isinstance(current, dict):
        return current
    return {}


def _page_set_from_hicache_snapshot(snapshot: Any, *, target_page_size: int | None = None) -> set[str]:
    """从 HiCache state snapshot 的 radix nodes 提取 page identity 集合。"""

    pages: set[str] = set()
    if not isinstance(snapshot, dict):
        return pages
    nodes = snapshot.get("nodes")
    if not isinstance(nodes, list):
        return pages
    for node in nodes:
        if not isinstance(node, dict):
            continue
        value = node.get("hash_value")
        if target_page_size is not None:
            target_hashes = node.get("target_hash_value_by_page_size")
            if isinstance(target_hashes, dict) and str(target_page_size) in target_hashes:
                value = target_hashes.get(str(target_page_size))
        if isinstance(value, list):
            pages.update(str(item) for item in value if item is not None)
        elif value is not None:
            pages.add(str(value))
    return pages


def _is_hicache_state_target_id(target_id: Any) -> bool:
    text = str(target_id or "").lower()
    return "hiradix" in text or "hicache" in text or "cache_controller" in text


def _hicache_target_page_sizes_from_targets(targets: list[dict[str, Any]]) -> list[int]:
    sizes: list[int] = []
    seen: set[int] = set()
    pattern = re.compile(r"page_hashes(?:_after_prefix|_concat)?:[^,\n]+(?:,[^,\n]+)*,(\d+)(?:,|$)")
    for target in targets:
        fields = target.get("fields") if isinstance(target.get("fields"), list) else []
        for field in fields:
            if not isinstance(field, dict):
                continue
            source = str(field.get("source") or "")
            for match in pattern.finditer(source):
                try:
                    size = int(match.group(1))
                except ValueError:
                    continue
                if size <= 0 or size in seen:
                    continue
                seen.add(size)
                sizes.append(size)
    return sizes


def materialize_hicache_radix_removed_pages(trace_dir: Path, *, target_page_size: int | None = None) -> dict[str, int]:
    """把同一 HiCache 调用 start/end snapshot 的 radix delta 写入模型输入事件。

    `hicache_state:self` 仍作为 validation-only snapshot 输出；这里仅在 profiling
    收尾阶段提取 operation-level `radix_removed_page_identity`。这样 C++ state
    model 消费的是明确的调用内事实，而不是完整 oracle state snapshot。
    """

    python_probe_dir = trace_dir / "python_probe"
    result = {
        "files_scanned": 0,
        "files_updated": 0,
        "end_events": 0,
        "insert_end_events": 0,
        "materialized_events": 0,
        "materialized_pages": 0,
        "target_materialized_events": 0,
        "target_materialized_pages": 0,
    }
    if not python_probe_dir.is_dir():
        return result

    for trace_path in sorted(python_probe_dir.glob("*.json")):
        result["files_scanned"] += 1
        payload = load_json(trace_path)
        events = payload.get("traceEvents")
        if not isinstance(events, list):
            continue

        start_pages_by_key: dict[tuple[Any, Any, Any, Any], set[str]] = {}
        end_pages_by_key: dict[tuple[Any, Any, Any, Any, Any], set[str]] = {}
        target_start_pages_by_key: dict[tuple[Any, Any, Any, Any], set[str]] = {}
        target_end_pages_by_key: dict[tuple[Any, Any, Any, Any, Any], set[str]] = {}
        for event in events:
            if not isinstance(event, dict):
                continue
            name = event.get("name")
            args = event.get("args") if isinstance(event.get("args"), dict) else {}
            target_id = args.get("target_id")
            if not isinstance(args, dict) or not _is_hicache_state_target_id(target_id):
                continue
            if isinstance(name, str) and name.endswith("_start:state_snapshot"):
                key = (event.get("pid"), event.get("tid"), event.get("ts"), target_id)
                snapshot = args.get("state_snapshot")
                start_pages_by_key[key] = _page_set_from_hicache_snapshot(snapshot)
                if target_page_size is not None:
                    target_start_pages_by_key[key] = _page_set_from_hicache_snapshot(snapshot, target_page_size=target_page_size)
            elif isinstance(name, str) and name.endswith("_end:state_snapshot"):
                key = (event.get("pid"), event.get("tid"), event.get("ts"), event.get("dur", 0), target_id)
                snapshot = args.get("state_snapshot")
                end_pages_by_key[key] = _page_set_from_hicache_snapshot(snapshot)
                if target_page_size is not None:
                    target_end_pages_by_key[key] = _page_set_from_hicache_snapshot(snapshot, target_page_size=target_page_size)

        updated = False
        for event in events:
            if not isinstance(event, dict):
                continue
            name = event.get("name")
            if not isinstance(name, str) or not name.startswith("hicache_") or not name.endswith("_end"):
                continue
            args = event.get("args") if isinstance(event.get("args"), dict) else None
            if not isinstance(args, dict) or not _is_hicache_state_target_id(args.get("target_id")):
                continue
            result["end_events"] += 1
            if args.get("target_id") == "hiradix.insert":
                result["insert_end_events"] += 1
            start_key = (event.get("pid"), event.get("tid"), event.get("ts"), args.get("target_id"))
            end_key = (event.get("pid"), event.get("tid"), event.get("ts"), event.get("dur", 0), args.get("target_id"))
            start_pages = start_pages_by_key.get(start_key)
            end_pages = end_pages_by_key.get(end_key)
            if start_pages is None or end_pages is None:
                continue
            current = args.get("radix_removed_page_identity")
            removed_pages = sorted(start_pages - end_pages)
            if removed_pages and not (isinstance(current, list) and current):
                args["radix_removed_page_identity"] = removed_pages
                result["materialized_events"] += 1
                result["materialized_pages"] += len(removed_pages)
                updated = True

            if target_page_size is not None:
                target_start_pages = target_start_pages_by_key.get(start_key)
                target_end_pages = target_end_pages_by_key.get(end_key)
                if target_start_pages is not None and target_end_pages is not None:
                    target_removed_pages = sorted(target_start_pages - target_end_pages)
                    if target_removed_pages:
                        args["target_radix_removed_page_identity"] = target_removed_pages
                        result["target_materialized_events"] += 1
                        result["target_materialized_pages"] += len(target_removed_pages)
                        updated = True

        if updated:
            dump_json(trace_path, payload)
            result["files_updated"] += 1
    return result


class ProfileRun:
    """单次 profiling 运行的执行器。"""

    def __init__(self, cfg: dict[str, Any], *, dry_run: bool) -> None:
        self.cfg = cfg
        self.dry_run = dry_run
        self.framework = str(cfg.get("framework", "sglang"))
        if self.framework != "sglang":
            raise ValueError("scripts/internal/profile_runner.py currently supports framework=sglang")

        self.runtime = normalize_profiling_config(cfg)
        self.layout = RunLayout.from_config(cfg, framework=self.framework)
        self.server_cfg = cfg.get("server", {})
        self.server_command = expand_command_placeholders(command_from_config(self.server_cfg["command"]), self.layout)
        self.bench_command = build_bench_command(cfg.get("bench", {}), self.layout)

    def run(self) -> Path:
        self.layout.prepare(clean=bool(self.cfg.get("clean_run_dir", False)))
        self._write_run_inputs()

        log(f"Run dir: {self.layout.run_dir}")
        started_at = time.time()
        if self.dry_run:
            self._write_manifest(started_at=started_at, status="dry_run", dry_run=True)
            return self.layout.run_dir

        status = "completed"
        error: str | None = None
        backup: ModelConfigBackup | None = None
        server_process: subprocess.Popen[Any] | None = None
        try:
            server_env = self._build_server_env()
            backup = apply_model_config_overrides(self.cfg, self.server_command, self.layout)

            log("Starting SGLang server.")
            server_process = start_process(self.server_command, self.layout.log_dir / "server.log", server_env)
            self._wait_for_server(server_process)
            log("Server is ready.")

            torch_profile_enabled = self._torch_profile_enabled()
            if torch_profile_enabled:
                self._start_torch_profiler()

            if self.bench_command is not None:
                self._run_bench(server_env)

            if torch_profile_enabled and self._should_stop_torch_profiler_after_workload():
                self._stop_torch_profiler()

        except Exception as exc:
            status = "failed"
            error = str(exc)
            raise
        finally:
            stop_process(server_process)
            restore_model_config(backup)
            if status == "completed" and self._hicache_state_trace_enabled():
                try:
                    target_page_sizes = self._hicache_target_page_sizes()
                    target_page_size = target_page_sizes[0] if len(target_page_sizes) == 1 else None
                    summary = materialize_hicache_radix_removed_pages(self.layout.trace_dir, target_page_size=target_page_size)
                    if summary["materialized_events"] > 0 or summary["target_materialized_events"] > 0:
                        log(
                            "Materialized HiCache radix removed pages: "
                            f"events={summary['materialized_events']} pages={summary['materialized_pages']} "
                            f"target_events={summary['target_materialized_events']} target_pages={summary['target_materialized_pages']}"
                        )
                except Exception as exc:
                    status = "failed"
                    error = f"postprocess hicache radix removed pages failed: {exc}"
            self._write_manifest(
                started_at=started_at,
                status=status,
                dry_run=False,
                error=error,
            )

        if status == "failed" and error:
            raise RuntimeError(error)
        log("Profile run completed.")
        return self.layout.run_dir

    def _write_run_inputs(self) -> None:
        dump_json(self.layout.run_dir / "config.json", self.cfg)
        (self.layout.run_dir / "server_cmd.txt").write_text(
            command_to_text(self.server_command) + "\n",
            encoding="utf-8",
        )
        if self.bench_command is not None:
            (self.layout.run_dir / "bench_cmd.txt").write_text(
                command_to_text(self.bench_command) + "\n",
                encoding="utf-8",
            )

    def _write_manifest(
        self,
        *,
        started_at: float,
        status: str,
        dry_run: bool,
        error: str | None = None,
    ) -> None:
        manifest = build_profile_manifest(
            run_dir=self.layout.run_dir,
            cfg=self.cfg,
            runtime=self.runtime,
            started_at=started_at,
            ended_at=time.time(),
            status=status,
            dry_run=dry_run,
            error=error,
        )
        dump_json(self.layout.run_dir / "profile_manifest.json", manifest)

    def _build_server_env(self) -> dict[str, str]:
        """构造 server 环境。

        Python probe 和 LD_PRELOAD 是两条独立路径：前者注入 PYTHONPATH 和
        TRACE_SIM_PYTHON_PROBE_*，后者只注入 LD_PRELOAD 与 HOOK_TRACE_OUTPUT。
        """

        env = os.environ.copy()
        for key, value in self.cfg.get("env", {}).items():
            env[str(key)] = expand_layout_placeholders(str(value), self.layout)

        self._apply_sglang_defaults(env)
        env["SGLANG_TORCH_PROFILER_DIR"] = str(self.layout.torch_trace_dir)
        env["TRACE_SIM_PROFILING_CHANNELS"] = ",".join(self.runtime.channels)
        env["TRACE_SIM_PROFILING_DEBUG"] = "1" if self.runtime.debug else "0"

        if self.runtime.enabled and "python_probe" in self.runtime.channels:
            self._apply_python_probe_env(env)
        if self.runtime.enabled and "ld_preload" in self.runtime.channels:
            self._apply_ld_preload_env(env)
        return env

    @staticmethod
    def _apply_sglang_defaults(env: dict[str, str]) -> None:
        """写入 SGLang / Ascend 常用默认值，但不覆盖用户显式 env。"""

        env.setdefault("HOOK_ASCENDCL_SO_PATH", "/usr/local/Ascend/ascend-toolkit/latest/lib64/libascendcl.so")
        env.setdefault("SGLANG_SET_CPU_AFFINITY", "1")
        env.setdefault("PYTORCH_NPU_ALLOC_CONF", "expandable_segments:True")
        env.setdefault("STREAMS_PER_DEVICE", "32")
        env.setdefault("HCCL_BUFFSIZE", "1536")
        env.setdefault("HCCL_OP_EXPANSION_MODE", "AIV")

    def _apply_python_probe_env(self, env: dict[str, str]) -> None:
        prepend_pythonpath(env, PYTHON_PROBE_ROOT)
        env["TRACE_SIM_PYTHON_PROBE"] = "1"
        env["TRACE_SIM_PYTHON_PROBES"] = ",".join(self.runtime.python_probes)
        env["TRACE_SIM_PYTHON_PROBE_TARGETS"] = json.dumps(
            self._python_probe_targets_for_env(),
            ensure_ascii=False,
        )
        env["TRACE_SIM_PYTHON_PROBE_OUTPUT"] = str(self.layout.trace_dir / "python_probe")
        if self._hicache_state_trace_enabled():
            env["TRACE_SIM_HICACHE_STATE_TRACE"] = "1"
            target_page_sizes = self._hicache_target_page_sizes()
            if target_page_sizes:
                env["TRACE_SIM_HICACHE_STATE_TARGET_PAGE_SIZES"] = ",".join(str(size) for size in target_page_sizes)
        if self.runtime.debug:
            env["TRACE_SIM_PYTHON_PROBE_DEBUG"] = "1"

    def _python_probe_targets_for_env(self) -> list[dict[str, Any]]:
        """按本次 profiling 配置生成真正注入 server 的 target。

        state_trace 是验证开关，不要求用户手动把 `hicache_state:self` 写进每个
        HiCache target。开启时给相关 target 追加 validation-only 字段；probe 会把
        它拆成 `model_input=false` 事件，真实执行事件仍进入 faithful replay。
        """

        targets = [dict(target) for target in self.runtime.python_targets]
        if not self._hicache_state_trace_enabled():
            return targets

        for target in targets:
            if not self._is_hicache_python_target(target):
                continue
            fields = [dict(field) for field in target.get("fields", []) if isinstance(field, dict)]
            if not any(field.get("source") == "hicache_state:self" for field in fields):
                state_field = {
                    "name": "state_snapshot",
                    "source": "hicache_state:self",
                    "required": False,
                }
                radix_removed_index = next(
                    (index for index, field in enumerate(fields) if field.get("source") == "hicache_radix_removed_pages:self"),
                    None,
                )
                if radix_removed_index is None:
                    fields.append(state_field)
                else:
                    fields.insert(radix_removed_index, state_field)
            target["fields"] = fields
        return targets

    def _hicache_state_trace_enabled(self) -> bool:
        python_probe = channel_config(self.cfg, "python_probe")
        state_trace = python_probe.get("state_trace") if isinstance(python_probe.get("state_trace"), dict) else {}
        return bool(state_trace.get("enabled", False))

    def _hicache_target_page_sizes(self) -> list[int]:
        return _hicache_target_page_sizes_from_targets(self.runtime.python_targets)

    @staticmethod
    def _is_hicache_python_target(target: dict[str, Any]) -> bool:
        module = str(target.get("module") or "")
        target_path = str(target.get("target") or "")
        target_id = str(target.get("id") or "")
        return "hicache" in target_id.lower() or "hiradix" in module.lower() or "cache_controller" in module.lower() or "HiCache" in target_path

    def _apply_ld_preload_env(self, env: dict[str, str]) -> None:
        ld_preload = self._ld_preload_cfg()
        if not ld_preload.get("enabled", True):
            return
        hook_lib = resolve_repo_path(ld_preload.get("library", "build/docker/sglang/lib/libhook.so"))
        if hook_lib is None or not hook_lib.is_file():
            raise FileNotFoundError(
                f"missing LD_PRELOAD library: {hook_lib}. Build it with scripts/internal/hooks/build.sh sglang."
            )
        env["LD_PRELOAD"] = str(hook_lib)
        env["HOOK_TRACE_OUTPUT"] = str(
            resolve_run_path(ld_preload.get("trace_output", "trace/ld_preload/cpu_trace.json"), self.layout.run_dir)
        )

    def _wait_for_server(self, process: subprocess.Popen[Any]) -> None:
        ready_url = self.server_cfg.get("ready_url", "http://127.0.0.1:30000/get_model_info")
        wait_for_ready(process, ready_url, int(self.server_cfg.get("ready_timeout_sec", 1800)))

    def _api_base(self) -> str:
        ready_url = self.server_cfg.get("ready_url", "http://127.0.0.1:30000/get_model_info")
        return self._torch_profile_cfg().get("api_base_url") or api_base_from_ready_url(ready_url)

    def _torch_profile_enabled(self) -> bool:
        profile = self._torch_profile_cfg()
        return self.runtime.enabled and "torch" in self.runtime.channels and profile.get("enabled", True)

    def _torch_profile_cfg(self) -> dict[str, Any]:
        return channel_config(self.cfg, "torch")

    def _ld_preload_cfg(self) -> dict[str, Any]:
        return channel_config(self.cfg, "ld_preload")

    def _should_stop_torch_profiler_after_workload(self) -> bool:
        """判断 runner 是否需要在 workload 结束后手动停止 profiler。

        默认语义是覆盖完整 workload：`/start_profile` 后运行 workload，workload 结束
        再调用 `/stop_profile`。如果用户显式配置 `num_steps`，SGLang/torch profiler
        会按 step 自动结束；这时 runner 不再强制 stop，避免 server 记录
        "Profiling is not in progress" 的 500 错误。
        """

        profile = self._torch_profile_cfg()
        if not profile.get("stop_after_workload", True):
            return False
        return profile.get("num_steps") is None

    def _start_torch_profiler(self) -> None:
        profile = self._torch_profile_cfg()
        body = build_profile_body(profile, self.layout)
        dump_json(self.layout.run_dir / "profile_start_body.json", body)
        log("Starting SGLang profiler via /start_profile.")
        response = post_json(
            self._api_base().rstrip("/") + "/start_profile",
            body,
            timeout=int(profile.get("start_timeout_sec", 120)),
        )
        dump_json(self.layout.run_dir / "profile_start_response.json", response)

    def _stop_torch_profiler(self) -> None:
        profile = self._torch_profile_cfg()
        log("Stopping SGLang profiler via /stop_profile.")
        try:
            response = post_json(
                self._api_base().rstrip("/") + "/stop_profile",
                None,
                timeout=int(profile.get("stop_timeout_sec", 1800)),
            )
        except Exception as exc:
            if profile.get("strict_stop", False):
                raise
            response = {"warning": str(exc)}
        dump_json(self.layout.run_dir / "profile_stop_response.json", response)

    def _run_bench(self, server_env: dict[str, str]) -> None:
        log("Running workload.")
        bench_env = server_env.copy()
        for key in BENCH_ENV_REMOVE_KEYS:
            bench_env.pop(key, None)
        remove_pythonpath_entry(bench_env, PYTHON_PROBE_ROOT)
        bench_proc = start_process(self.bench_command, self.layout.log_dir / "bench.log", bench_env)
        bench_code = bench_proc.wait()
        if bench_code != 0:
            raise RuntimeError(f"bench command failed, code={bench_code}")


def run_profile(cfg: dict[str, Any], dry_run: bool) -> Path:
    return ProfileRun(cfg, dry_run=dry_run).run()


def expand_suite(cfg: dict[str, Any]) -> list[dict[str, Any]]:
    experiments = cfg.get("experiments")
    if experiments is None:
        return [cfg]
    if not isinstance(experiments, list) or not experiments:
        raise ValueError("experiments must be a non-empty list")

    common = {key: value for key, value in cfg.items() if key not in INTERNAL_SUITE_KEYS}
    expanded = []
    for index, experiment in enumerate(experiments, start=1):
        if not isinstance(experiment, dict):
            raise TypeError(f"experiments[{index - 1}] must be an object")
        merged = deep_merge(common, experiment)
        apply_unset(merged, experiment.get("$unset"))
        merged.setdefault("name", f"experiment-{index}")
        expanded.append(merged)
    return expanded


def run_profile_suite(cfg: dict[str, Any], dry_run: bool) -> list[Path]:
    experiments = expand_suite(cfg)
    if len(experiments) == 1 and "experiments" not in cfg:
        return [run_profile(experiments[0], dry_run)]

    framework = cfg.get("framework", "sglang")
    suite_name = sanitize(str(cfg.get("name", f"{framework}-profile-suite")))
    suite_root = resolve_repo_path(cfg.get("run_root")) or ROOT_DIR / "data/profile_runs" / str(framework)
    suite_id = cfg.get("run_id") or f"{time.strftime('%Y%m%d_%H%M%S')}_{suite_name}"
    suite_dir = suite_root / sanitize(str(suite_id))
    suite_dir.mkdir(parents=True, exist_ok=True)
    dump_json(suite_dir / "suite_config.json", cfg)

    log(f"Suite dir: {suite_dir}")
    run_dirs: list[Path] = []
    failures: list[dict[str, str]] = []
    continue_on_error = bool(cfg.get("continue_on_error", False))
    for index, experiment in enumerate(experiments, start=1):
        exp_name = sanitize(str(experiment.get("name", f"experiment-{index}")))
        exp_cfg = dict(experiment)
        exp_cfg["run_root"] = str(suite_dir)
        exp_cfg["run_id"] = f"{index:02d}_{exp_name}"

        log(f"Suite experiment {index}/{len(experiments)}: {exp_name}")
        try:
            run_dirs.append(run_profile(exp_cfg, dry_run))
        except Exception as exc:
            failures.append({"name": exp_name, "error": str(exc)})
            if not continue_on_error:
                raise

    dump_json(
        suite_dir / "suite_result.json",
        {
            "suite_dir": str(suite_dir),
            "runs": [str(path) for path in run_dirs],
            "failures": failures,
        },
    )
    return run_dirs


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run SGLang profiling experiments.")
    parser.add_argument("--config", required=True, help="JSON profile config path")
    parser.add_argument("--dry-run", action="store_true", help="expand config and manifest without starting the server")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    config_path = resolve_repo_path(args.config)
    if config_path is None or not config_path.is_file():
        raise FileNotFoundError(f"missing config: {args.config}")
    run_dirs = run_profile_suite(load_json(config_path), args.dry_run)
    for run_dir in run_dirs:
        print(run_dir)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
