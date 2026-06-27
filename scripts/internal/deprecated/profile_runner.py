#!/usr/bin/env python3
"""SGLang profiling runner。

本脚本只负责启动被测进程、注入采集环境、运行 workload，并写出 profile manifest。
建模判断不放在这里，避免 profiling 阶段和 modeling 阶段互相污染。
"""

from __future__ import annotations

import argparse
import copy
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
from profiling.python_probe.trace_sim_probe.schema import validate_hicache_fact  # noqa: E402


INTERNAL_SUITE_KEYS = {"experiments", "matrix", "continue_on_error", "$unset"}
MATRIX_ENTRY_META_KEYS = {"id", "name", "description", "$unset"}
EXPERIMENT_REF_KEYS = {"server_ref", "input_ref"}
PROFILE_EXPERIMENTS_ENV = "TRACE_SIM_PROFILE_EXPERIMENTS"
PROFILE_INPUTS_ENV = "TRACE_SIM_PROFILE_INPUTS"
PROFILE_SERVERS_ENV = "TRACE_SIM_PROFILE_SERVERS"
PROFILE_FORCED_TOKEN_BUNDLE_ENV = "TRACE_SIM_FORCED_TOKEN_BUNDLE"
PYTHON_PROBE_ROOT = ROOT_DIR / "src/profiling/python_probe"
DEFAULT_HICACHE_TARGET_CATALOG = ROOT_DIR / "configs/profiling/hicache_probe_targets.json"
BENCH_SCRIPT_ROOT = ROOT_DIR / "scripts/bench"
BENCH_ENV_REMOVE_KEYS = (
    "LD_PRELOAD",
    "HOOK_TRACE_OUTPUT",
    "TRACE_SIM_PYTHON_PROBE",
    "TRACE_SIM_PYTHON_PROBES",
    "TRACE_SIM_PYTHON_PROBE_TARGETS",
    "TRACE_SIM_PYTHON_PROBE_OUTPUT",
    "TRACE_SIM_PYTHON_PROBE_DEBUG",
    "TRACE_SIM_PYTHON_PROBE_FLUSH_EVERY",
    "TRACE_SIM_HICACHE_CONSUMERS",
    "TRACE_SIM_HICACHE_INTERNAL_HOOKS",
)

if str(Path(__file__).resolve().parent) not in sys.path:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
if str(BENCH_SCRIPT_ROOT) not in sys.path:
    sys.path.insert(0, str(BENCH_SCRIPT_ROOT))

from hicache_forced_token_contract import (  # noqa: E402
    FORCED_TOKEN_BUNDLE_SCHEMA,
    FORCED_TOKEN_ERROR_CAPTURE_OVERWRITE,
    FORCED_TOKEN_ERROR_BUNDLE_PROVENANCE,
    FORCED_TOKEN_ERROR_PLAN_MISSING,
    forced_token_bundle_summary,
    forced_token_plan_summary,
    forced_token_quality_from_workload_report,
    load_forced_token_plan,
    resolve_forced_token_bundle_plan,
    sha256_file,
    sha256_json,
    validate_plan_contract,
)
from hicache_phased_workload import (  # noqa: E402
    build_arg_parser as build_hicache_workload_arg_parser,
    build_plan as build_hicache_workload_plan,
    logical_request_id as hicache_logical_request_id,
    workload_args_digest as hicache_workload_args_digest,
    workload_id as hicache_workload_id,
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
        """从运行配置生成稳定目录布局。"""

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
        """创建本次 run 需要的目录，必要时清理旧 run 目录。"""

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
    """输出带时间戳的 runner 日志。"""

    timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
    print(f"[{timestamp}] {message}", flush=True)


def sanitize(value: str) -> str:
    """把用户可配置名称规整成可作为目录名的短字符串。"""

    value = re.sub(r"[^A-Za-z0-9_.-]+", "_", value.strip())
    return value.strip("._-") or "profile"


def load_json(path: Path) -> dict[str, Any]:
    """读取 UTF-8 JSON 对象配置。"""

    with path.open("r", encoding="utf-8") as file_obj:
        return json.load(file_obj)


def dump_json(path: Path, value: Any) -> None:
    """写入格式化 JSON，并保证父目录存在。"""

    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as file_obj:
        json.dump(value, file_obj, indent=2, ensure_ascii=False)
        file_obj.write("\n")


def resolve_repo_path(value: str | None) -> Path | None:
    """把配置路径解析为 repo 内绝对路径。"""

    if not value:
        return None
    path = Path(value).expanduser()
    if path.is_absolute():
        return path
    return ROOT_DIR / path


def resolve_run_path(value: str | None, run_dir: Path) -> Path | None:
    """把 run-dir 相对路径解析为绝对路径。"""

    if not value:
        return None
    path = Path(value).expanduser()
    if path.is_absolute():
        return path
    return run_dir / path


def deep_merge(base: dict[str, Any], override: dict[str, Any]) -> dict[str, Any]:
    """递归合并 suite common 配置和单个 experiment 配置。"""

    merged = copy.deepcopy(base)
    for key, value in override.items():
        if key == "$unset":
            continue
        if key in merged and isinstance(merged[key], dict) and isinstance(value, dict):
            merged[key] = deep_merge(merged[key], value)
        else:
            merged[key] = copy.deepcopy(value)
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
    """应用 suite `$unset` 列表，删除已合并配置中的字段。"""

    if paths is None:
        return
    if not isinstance(paths, list) or not all(isinstance(path, str) for path in paths):
        raise TypeError("$unset must be a list of dot-separated paths")
    for path in paths:
        delete_path(value, path)


def command_from_config(command: Any) -> list[str] | str:
    """校验并返回配置中的命令表达。"""

    if isinstance(command, list) and all(isinstance(item, str) for item in command):
        return command
    if isinstance(command, str):
        return command
    raise TypeError("command must be either a string or a list of strings")


def command_to_text(command: list[str] | str) -> str:
    """把命令转换成可写入审计文件的文本形式。"""

    if isinstance(command, list):
        return shlex.join(command)
    return command


CONFIG_PLACEHOLDER_ROOTS = {"metadata", "server", "bench", "env", "modeling"}


def config_placeholder_value(cfg: dict[str, Any], path: str) -> str | None:
    """解析 `{metadata.foo}` 等配置占位符的替换值。"""

    parts = [part for part in path.split(".") if part]
    if len(parts) < 2 or parts[0] not in CONFIG_PLACEHOLDER_ROOTS:
        return None
    cursor: Any = cfg
    for part in parts:
        if not isinstance(cursor, dict) or part not in cursor:
            return None
        cursor = cursor[part]
    if isinstance(cursor, (dict, list)):
        return json.dumps(cursor, ensure_ascii=False, sort_keys=True)
    return str(cursor)


def expand_config_placeholders(value: str, cfg: dict[str, Any]) -> str:
    """替换 `{metadata.foo}` 这类点分配置占位符。

    suite input 可以引用 server matrix 合并后的 metadata/env 字段，避免为了
    一个 server policy 参数复制 input 配置。普通 JSON 字符串里的 `{...}` 不含
    点分路径，不会被这里匹配。
    """

    pattern = re.compile(r"\{([A-Za-z_][A-Za-z0-9_-]*(?:\.[A-Za-z0-9_-]+)+)\}")

    def replace(match: re.Match[str]) -> str:
        path = match.group(1)
        replacement = config_placeholder_value(cfg, path)
        if replacement is None:
            raise ValueError(f"unknown config placeholder: {{{path}}}")
        return replacement

    return pattern.sub(replace, value)


def expand_command_placeholders(command: list[str] | str, layout: RunLayout, cfg: dict[str, Any] | None = None) -> list[str] | str:
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
        if cfg is not None:
            value = expand_config_placeholders(value, cfg)
        return value

    if isinstance(command, list):
        return [expand(item) for item in command]
    return expand(command)


def expand_layout_placeholders(value: str, layout: RunLayout, cfg: dict[str, Any] | None = None) -> str:
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
    if cfg is not None:
        result = expand_config_placeholders(result, cfg)
    return result


def expand_runtime_value(value: Any, layout: RunLayout, cfg: dict[str, Any]) -> Any:
    """递归展开 runtime 配置中的 run-dir 和配置占位符。"""

    if isinstance(value, str):
        return expand_layout_placeholders(value, layout, cfg)
    if isinstance(value, list):
        return [expand_runtime_value(item, layout, cfg) for item in value]
    return value


def append_cli_arg(command: list[str], key: str, value: Any) -> None:
    """按 bench_serving 约定把 JSON 参数追加为 CLI 选项。"""

    option = "--" + key.replace("_", "-")
    if isinstance(value, bool):
        if value:
            command.append(option)
    elif isinstance(value, list):
        for item in value:
            command.extend([option, str(item)])
    elif value is not None:
        command.extend([option, str(value)])


def build_bench_command(bench: dict[str, Any], layout: RunLayout, cfg: dict[str, Any]) -> list[str] | str | None:
    """从配置构造 workload driver 命令。

    显式 `bench.command` 优先；否则按 SGLang bench_serving 的参数对象生成命令。
    """

    if not bench:
        return None
    if "command" in bench:
        return expand_command_placeholders(command_from_config(bench["command"]), layout, cfg)

    kind = bench.get("kind", "sglang.bench_serving")
    if kind != "sglang.bench_serving":
        raise ValueError(f"unknown bench kind: {kind}")

    args = dict(bench.get("args", {}))
    args.setdefault("output_file", bench.get("output_file") or str(layout.bench_dir / "bench.jsonl"))

    command = ["python3", "-m", "sglang.bench_serving"]
    for key, value in args.items():
        append_cli_arg(command, key, expand_runtime_value(value, layout, cfg))
    return command


def command_tokens(command: list[str] | str | None) -> list[str]:
    """把命令规整为 token list，解析失败时返回空列表。"""

    if command is None:
        return []
    if isinstance(command, list):
        return list(command)
    try:
        return shlex.split(command)
    except ValueError:
        return []


def hicache_workload_argv(tokens: list[str]) -> list[str] | None:
    """从命令中提取 hicache_phased_workload.py 的参数段。"""

    for index, token in enumerate(tokens):
        if token.endswith("hicache_phased_workload.py"):
            return tokens[index + 1 :]
    return None


def parse_hicache_workload_args(command: list[str] | str | None) -> argparse.Namespace | None:
    """解析 HiCache phased workload 参数；其他 workload 返回 None。"""

    argv = hicache_workload_argv(command_tokens(command))
    if argv is None:
        return None
    try:
        return build_hicache_workload_arg_parser().parse_args(argv)
    except SystemExit as exc:
        raise ValueError(f"invalid hicache phased workload command, argparse_exit={exc.code}") from None


def forced_token_plan_path_from_args(args: argparse.Namespace) -> Path | None:
    """按 workload CLI 语义解析 forced-token plan 路径。"""

    if args.forced_token_plan:
        return resolve_repo_path(str(args.forced_token_plan))
    if args.forced_token_mode == "capture":
        return Path(str(args.output_dir)) / "forced_token_plan.json"
    return None


def forced_token_contract_report(
    bench_command: list[str] | str | None,
    bundle_provenance: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """解析并校验一次 workload 命令中的 forced-token contract。"""

    workload_args = parse_hicache_workload_args(bench_command)
    if workload_args is None:
        return {
            "enabled": False,
            "mode": "none",
            "errors": [],
        }

    mode = str(workload_args.forced_token_mode)
    workload_plan = build_hicache_workload_plan(workload_args)
    plan_path = forced_token_plan_path_from_args(workload_args)
    report: dict[str, Any] = {
        "enabled": mode != "none",
        "mode": mode,
        "errors": [],
        "workload_id": hicache_workload_id(workload_args),
        "workload_fingerprint": hicache_workload_args_digest(workload_args),
        "workload_request_count": len(workload_plan),
        "plan_path": str(plan_path) if plan_path is not None else None,
        "plan": None,
        "bundle": bundle_provenance,
    }
    if mode == "none":
        return report
    if plan_path is None:
        report["errors"] = [FORCED_TOKEN_ERROR_PLAN_MISSING]
        return report

    if mode == "capture":
        report["plan"] = forced_token_plan_summary(plan_path).to_dict()
        if plan_path.exists():
            report["errors"] = [FORCED_TOKEN_ERROR_CAPTURE_OVERWRITE]
            return report
        return report

    if mode != "replay":
        report["errors"] = [f"unsupported_forced_token_mode:{mode}"]
        return report

    summary = forced_token_plan_summary(plan_path)
    report["plan"] = summary.to_dict()
    if not summary.exists:
        report["errors"] = [FORCED_TOKEN_ERROR_PLAN_MISSING]
        return report
    try:
        plan = load_forced_token_plan(plan_path)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        report["errors"] = [f"forced_token_plan_invalid:{exc}"]
        return report

    expected_request_ids = [
        hicache_logical_request_id(workload_args, item, sequence_id)
        for sequence_id, item in enumerate(workload_plan)
    ]
    report["errors"] = validate_plan_contract(
        plan,
        workload_id=hicache_workload_id(workload_args),
        workload_fingerprint=hicache_workload_args_digest(workload_args),
        expected_request_ids=expected_request_ids,
    )
    if bundle_provenance:
        if bundle_provenance.get("plan_sha256") != summary.sha256:
            report["errors"].append(FORCED_TOKEN_ERROR_BUNDLE_PROVENANCE)
        report["errors"] = sorted(set(report["errors"]))
    return report


def forced_token_mode_from_config(cfg: dict[str, Any]) -> str:
    """读取已展开 experiment 的 forced-token workload mode。"""

    bench = cfg.get("bench") if isinstance(cfg.get("bench"), dict) else {}
    command = command_from_config(bench.get("command")) if "command" in bench else None
    tokens = command_tokens(command)
    if "--forced-token-mode" not in tokens:
        return "none"
    index = tokens.index("--forced-token-mode")
    if index + 1 >= len(tokens):
        raise ValueError("--forced-token-mode is missing its value")
    return str(tokens[index + 1])


def inject_forced_token_bundle_plan(
    cfg: dict[str, Any],
    bundle_path: Path | None,
) -> dict[str, Any]:
    """按 suite input 从显式 bundle 注入 replay plan 和 provenance。"""

    result = copy.deepcopy(cfg)
    mode = forced_token_mode_from_config(result)
    metadata = result.get("metadata") if isinstance(result.get("metadata"), dict) else {}
    bench = result.get("bench") if isinstance(result.get("bench"), dict) else {}
    raw_command = command_from_config(bench.get("command")) if "command" in bench else None

    if mode != "replay":
        if bundle_path is not None:
            raise ValueError("--forced-token-bundle can only be used with forced-token replay experiments")
        return result
    if bundle_path is None:
        raise ValueError("forced-token replay requires --forced-token-bundle")

    input_id = str(metadata.get("suite_input_id") or "")
    if not input_id:
        raise ValueError("forced-token replay experiment is missing metadata.suite_input_id")
    resolved = resolve_forced_token_bundle_plan(bundle_path, input_id)
    plan_path = resolved.plan_path
    tokens = command_tokens(raw_command)
    if not tokens:
        raise ValueError("forced-token replay requires an explicit bench.command")
    if "--forced-token-plan" not in tokens:
        raise ValueError("forced-token replay config must contain --forced-token-plan {forced_token_plan}")
    index = tokens.index("--forced-token-plan")
    if index + 1 >= len(tokens) or tokens[index + 1] != "{forced_token_plan}":
        raise ValueError("forced-token replay config must use --forced-token-plan {forced_token_plan}")
    tokens[index + 1] = plan_path
    result.setdefault("bench", {})["command"] = tokens
    result.setdefault("metadata", {})["forced_token_bundle"] = resolved.to_dict()
    return result


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
    """恢复 profiling 前临时覆盖的模型 config.json。"""

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
    """从 ready URL 提取 SGLang API base URL。"""

    parsed = urllib.parse.urlsplit(ready_url)
    if not parsed.scheme or not parsed.netloc:
        raise ValueError(f"ready_url must be absolute: {ready_url}")
    return urllib.parse.urlunsplit((parsed.scheme, parsed.netloc, "", "", ""))


def post_json(url: str, body: dict[str, Any] | None, timeout: int = 60) -> Any:
    """向 SGLang profiler API 发送 JSON POST 并宽松解析响应。"""

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
    """终止 server 进程组，超时后升级为 SIGKILL。"""

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
    """构造传给 SGLang `/start_profile` 的请求体。"""

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


class ProfileRun:
    """单次 profiling 运行的执行器。"""

    def __init__(self, cfg: dict[str, Any], *, dry_run: bool) -> None:
        """规整单次 run 的配置、目录和 server/workload 命令。"""

        self.cfg = cfg
        self.dry_run = dry_run
        self.framework = str(cfg.get("framework", "sglang"))
        if self.framework != "sglang":
            raise ValueError("scripts/internal/profile_runner.py currently supports framework=sglang")

        self.runtime = normalize_profiling_config(cfg)
        self.layout = RunLayout.from_config(cfg, framework=self.framework)
        self.server_cfg = cfg.get("server", {})
        self.server_command = expand_command_placeholders(command_from_config(self.server_cfg["command"]), self.layout, self.cfg)
        self.bench_command = build_bench_command(cfg.get("bench", {}), self.layout, self.cfg)

    def run(self) -> Path:
        """执行一次 profiling run，并在 finally 中写出 manifest。"""

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
        """保存本次运行的原始配置和展开后的命令，供复现使用。"""

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
        """写出 profile manifest，作为后续 merge/modeling 的入口。"""

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
            env[str(key)] = expand_layout_placeholders(str(value), self.layout, self.cfg)

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
        """注入 Python probe 环境变量和 target 配置。"""

        python_probe = channel_config(self.cfg, "python_probe")
        selected_targets = self._python_probe_targets_for_env()
        prepend_pythonpath(env, PYTHON_PROBE_ROOT)
        env["TRACE_SIM_PYTHON_PROBE"] = "1"
        env["TRACE_SIM_PYTHON_PROBES"] = ",".join(self.runtime.python_probes)
        env["TRACE_SIM_PYTHON_PROBE_TARGETS"] = json.dumps(selected_targets, ensure_ascii=False)
        env["TRACE_SIM_PYTHON_PROBE_OUTPUT"] = str(self.layout.trace_dir / "python_probe")
        env["TRACE_SIM_HICACHE_CONSUMERS"] = ",".join(self.runtime.python_consumers)
        flush_every = python_probe.get("flush_every", python_probe.get("flush_interval_events"))
        if flush_every is not None:
            env["TRACE_SIM_PYTHON_PROBE_FLUSH_EVERY"] = str(flush_every)
        if "internal_hooks" in python_probe:
            env["TRACE_SIM_HICACHE_INTERNAL_HOOKS"] = "1" if bool(python_probe.get("internal_hooks")) else "0"
        if self.runtime.debug:
            env["TRACE_SIM_PYTHON_PROBE_DEBUG"] = "1"

    def _python_probe_targets_for_env(self) -> list[dict[str, Any]]:
        """按 requested consumers 从 target catalog 选择真正注入 server 的 targets。"""

        catalog_path = resolve_repo_path(self.runtime.python_target_catalog) or DEFAULT_HICACHE_TARGET_CATALOG
        raw_targets = load_json(catalog_path)
        if not isinstance(raw_targets, list):
            raise ValueError(f"python probe target catalog must be a JSON array: {catalog_path}")

        selected: list[dict[str, Any]] = []
        requested = tuple(self.runtime.python_consumers)
        for index, raw_target in enumerate(raw_targets):
            target = self._validated_catalog_target(raw_target, index, catalog_path)
            fact = target["fact"]
            allowed = set(fact["consumers"])
            selected_consumers = [consumer for consumer in requested if consumer in allowed]
            if not selected_consumers:
                continue
            selected_fact = {
                "class": fact["class"],
                "role": fact["role"],
                "consumers": selected_consumers,
            }
            target["fact"] = selected_fact
            selected.append(target)
        return selected

    @staticmethod
    def _validated_catalog_target(raw: Any, index: int, catalog_path: Path) -> dict[str, Any]:
        """Validate and clone a target catalog entry."""

        if not isinstance(raw, dict):
            raise ValueError(f"{catalog_path}: targets[{index}] must be an object")
        target = copy.deepcopy(raw)
        prefix = f"{catalog_path}: targets[{index}]"
        for key in ("id", "module", "target"):
            if not isinstance(target.get(key), str) or not target.get(key):
                raise ValueError(f"{prefix}.{key} must be a non-empty string")
        events = target.get("events")
        if not isinstance(events, list) or not all(isinstance(item, str) and item for item in events):
            raise ValueError(f"{prefix}.events must be an array of non-empty strings")
        fact = target.get("fact")
        if not isinstance(fact, dict):
            raise ValueError(f"{prefix}.fact must be an object")
        if set(fact) != {"class", "role", "consumers"}:
            raise ValueError(f"{prefix}.fact must contain only class, role, and consumers")
        fact_class = fact.get("class")
        role = fact.get("role")
        consumers = fact.get("consumers")
        if not isinstance(fact_class, str) or not fact_class:
            raise ValueError(f"{prefix}.fact.class must be a non-empty string")
        if not isinstance(role, str) or not role:
            raise ValueError(f"{prefix}.fact.role must be a non-empty string")
        if not isinstance(consumers, list) or not all(isinstance(item, str) and item for item in consumers):
            raise ValueError(f"{prefix}.fact.consumers must be a non-empty string array")
        validate_hicache_fact(fact_class, role, consumers)
        return target

    def _apply_ld_preload_env(self, env: dict[str, str]) -> None:
        """注入 LD_PRELOAD hook，并把输出固定到本次 run 目录。"""

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
        """等待 server ready，超时时暴露进程提前退出或健康检查失败。"""

        ready_url = self.server_cfg.get("ready_url", "http://127.0.0.1:30000/get_model_info")
        wait_for_ready(process, ready_url, int(self.server_cfg.get("ready_timeout_sec", 1800)))

    def _api_base(self) -> str:
        """返回 profiler API base，默认从 ready_url 推导。"""

        ready_url = self.server_cfg.get("ready_url", "http://127.0.0.1:30000/get_model_info")
        return self._torch_profile_cfg().get("api_base_url") or api_base_from_ready_url(ready_url)

    def _torch_profile_enabled(self) -> bool:
        """判断本次 run 是否调用 SGLang torch profiler API。"""

        profile = self._torch_profile_cfg()
        return self.runtime.enabled and "torch" in self.runtime.channels and profile.get("enabled", True)

    def _torch_profile_cfg(self) -> dict[str, Any]:
        """读取 torch profiler 渠道配置。"""

        return channel_config(self.cfg, "torch")

    def _ld_preload_cfg(self) -> dict[str, Any]:
        """读取 LD_PRELOAD 渠道配置。"""

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
        """调用 `/start_profile` 并保存请求体和响应。"""

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
        """调用 `/stop_profile`，非 strict 模式下把错误降级成响应记录。"""

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
        """运行 workload，并移除只应注入 server 的采集环境。"""

        log("Running workload.")
        bench_env = server_env.copy()
        for key in BENCH_ENV_REMOVE_KEYS:
            bench_env.pop(key, None)
        remove_pythonpath_entry(bench_env, PYTHON_PROBE_ROOT)
        metadata = self.cfg.get("metadata") if isinstance(self.cfg.get("metadata"), dict) else {}
        bench_env["TRACE_SIM_PROFILE_RUN_DIR"] = str(self.layout.run_dir)
        bench_env["TRACE_SIM_PROFILE_RUN_ID"] = str(self.cfg.get("run_id") or self.cfg.get("id") or self.cfg.get("name") or "")
        bench_env["TRACE_SIM_PROFILE_MANIFEST_PATH"] = str(self.layout.run_dir / "profile_manifest.json")
        if metadata.get("suite_server_id"):
            bench_env["TRACE_SIM_PROFILE_CONFIG_ID"] = str(metadata["suite_server_id"])
        if metadata.get("suite_input_id"):
            bench_env["TRACE_SIM_PROFILE_INPUT_ID"] = str(metadata["suite_input_id"])
        forced_token_bundle = metadata.get("forced_token_bundle")
        if isinstance(forced_token_bundle, dict):
            bundle_env = {
                "TRACE_SIM_FORCED_TOKEN_BUNDLE_PATH": forced_token_bundle.get("path"),
                "TRACE_SIM_FORCED_TOKEN_BUNDLE_SCHEMA": forced_token_bundle.get("schema"),
                "TRACE_SIM_FORCED_TOKEN_BUNDLE_SHA256": forced_token_bundle.get("sha256"),
                "TRACE_SIM_FORCED_TOKEN_BUNDLE_ID": forced_token_bundle.get("bundle_id"),
                "TRACE_SIM_FORCED_TOKEN_BUNDLE_PLAN_SHA256": forced_token_bundle.get("plan_sha256"),
            }
            for key, value in bundle_env.items():
                if value:
                    bench_env[key] = str(value)
        model_path = self.cfg.get("model_path") or self.server_cfg.get("model_path") or parse_model_path(self.server_command)
        if model_path:
            bench_env["TRACE_SIM_PROFILE_MODEL_PATH"] = str(model_path)
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
    bundle = metadata.get("forced_token_bundle")
    bundle_provenance = bundle if isinstance(bundle, dict) else None
    report = forced_token_contract_report(probe.bench_command, bundle_provenance)
    forced_errors = [str(error) for error in report.get("errors", [])]
    if forced_errors:
        exp_id = cfg.get("id") or cfg.get("name") or "profile"
        raise ValueError(
            f"forced token preflight failed for {exp_id}: {', '.join(forced_errors)}"
        )
    return report


def parse_experiment_selection(raw_values: list[str] | None, env_value: str | None = None) -> set[str]:
    """解析命令行或环境变量中的实验选择器。

    手动 profiling 时最常见的输入是 `--experiments a,b` 或重复
    `--experiment a --experiment b`。这里统一拆成去重集合，供
    `--list-experiments` 和真实运行共用同一套匹配逻辑。
    """

    selected: set[str] = set()
    for raw in [*(raw_values or []), env_value or ""]:
        for item in str(raw).split(","):
            item = item.strip()
            if item:
                selected.add(item)
    return selected


def experiment_identity(cfg: dict[str, Any], index: int) -> str:
    """返回实验的稳定 id/name，缺省时使用序号兜底。"""

    for key in ("id", "name"):
        value = cfg.get(key)
        if isinstance(value, str) and value.strip():
            return value.strip()
    return f"experiment-{index}"


def experiment_selectors(cfg: dict[str, Any], index: int) -> set[str]:
    """返回可用于命令行选择同一个实验的稳定选择器。"""

    selectors = {str(index), f"{index:02d}"}
    for key in ("id", "name"):
        value = cfg.get(key)
        if isinstance(value, str) and value.strip():
            selectors.add(value.strip())
            selectors.add(sanitize(value))
    return selectors


def describe_suite_experiment(index: int, cfg: dict[str, Any]) -> dict[str, Any]:
    """生成 `--list-experiments` 和 suite_selection 使用的实验摘要。"""

    metadata = cfg.get("metadata") if isinstance(cfg.get("metadata"), dict) else {}
    return {
        "index": index,
        "id": cfg.get("id", experiment_identity(cfg, index)),
        "name": cfg.get("name", experiment_identity(cfg, index)),
        "server_id": metadata.get("suite_server_id"),
        "input_id": metadata.get("suite_input_id"),
        "profile_mode": metadata.get("profile_mode"),
        "selectors": sorted(experiment_selectors(cfg, index)),
    }


def reject_profiling_override(value: dict[str, Any], context: str) -> None:
    """禁止 suite 局部覆盖 profiling 配置，保证采集合同在 suite 内一致。"""

    if "profiling" in value:
        raise ValueError(f"{context} must not override profiling; suite experiments share one profiling config")
    unset_paths = value.get("$unset")
    if isinstance(unset_paths, list):
        for path in unset_paths:
            if isinstance(path, str) and (path == "profiling" or path.startswith("profiling.")):
                raise ValueError(f"{context} must not unset profiling; suite experiments share one profiling config")


def matrix_entries(matrix: dict[str, Any], key: str) -> dict[str, dict[str, Any]]:
    """读取并校验 matrix.servers 或 matrix.inputs 列表。"""

    raw_entries = matrix.get(key)
    if not isinstance(raw_entries, list) or not raw_entries:
        raise ValueError(f"matrix.{key} must be a non-empty list")

    entries: dict[str, dict[str, Any]] = {}
    for index, entry in enumerate(raw_entries):
        if not isinstance(entry, dict):
            raise TypeError(f"matrix.{key}[{index}] must be an object")
        reject_profiling_override(entry, f"matrix.{key}[{index}]")
        raw_id = entry.get("id")
        if not isinstance(raw_id, str) or not raw_id.strip():
            raise ValueError(f"matrix.{key}[{index}].id must be a non-empty string")
        entry_id = raw_id.strip()
        if entry_id in entries:
            raise ValueError(f"duplicate matrix.{key} id: {entry_id}")
        entries[entry_id] = entry
    return entries


def matrix_entry_override(entry: dict[str, Any]) -> dict[str, Any]:
    """去掉 matrix entry 元字段，只保留参与配置合并的覆盖项。"""

    return {key: value for key, value in entry.items() if key not in MATRIX_ENTRY_META_KEYS}


def attach_suite_metadata(
    cfg: dict[str, Any],
    *,
    experiment_id: str,
    server_id: str | None,
    input_id: str | None,
) -> None:
    """给展开后的实验补充 suite 来源 metadata。"""

    metadata = cfg.get("metadata")
    if metadata is None:
        metadata = {}
    if not isinstance(metadata, dict):
        raise TypeError("metadata must be an object")
    metadata = dict(metadata)
    metadata.setdefault("suite_experiment_id", experiment_id)
    if server_id is not None:
        metadata.setdefault("suite_server_id", server_id)
    if input_id is not None:
        metadata.setdefault("suite_input_id", input_id)
    cfg["metadata"] = metadata


def generated_matrix_experiments(matrix: dict[str, Any]) -> list[dict[str, Any]]:
    """在未显式列出 experiments 时生成 server/input 笛卡尔积。"""

    servers = matrix_entries(matrix, "servers")
    inputs = matrix_entries(matrix, "inputs")
    experiments: list[dict[str, Any]] = []
    for server_id in servers:
        for input_id in inputs:
            experiment_id = f"{server_id}_{input_id}"
            experiments.append(
                {
                    "id": experiment_id,
                    "name": experiment_id,
                    "server_ref": server_id,
                    "input_ref": input_id,
                }
            )
    return experiments


def expand_matrix_experiment(
    common: dict[str, Any],
    matrix: dict[str, Any],
    experiment: dict[str, Any],
    index: int,
) -> dict[str, Any]:
    """把一个 matrix experiment 展开为可直接运行的单次配置。"""

    servers = matrix_entries(matrix, "servers")
    inputs = matrix_entries(matrix, "inputs")

    server_ref = experiment.get("server_ref")
    input_ref = experiment.get("input_ref")
    if not isinstance(server_ref, str) or not server_ref.strip():
        raise ValueError(f"experiments[{index - 1}].server_ref must reference matrix.servers")
    if not isinstance(input_ref, str) or not input_ref.strip():
        raise ValueError(f"experiments[{index - 1}].input_ref must reference matrix.inputs")
    server_id = server_ref.strip()
    input_id = input_ref.strip()
    if server_id not in servers:
        raise ValueError(f"experiments[{index - 1}].server_ref references unknown server: {server_id}")
    if input_id not in inputs:
        raise ValueError(f"experiments[{index - 1}].input_ref references unknown input: {input_id}")

    experiment_id = str(experiment.get("id") or f"{server_id}_{input_id}").strip()
    if not experiment_id:
        raise ValueError(f"experiments[{index - 1}].id must not be empty")
    merged = deep_merge(common, matrix_entry_override(servers[server_id]))
    apply_unset(merged, servers[server_id].get("$unset"))
    merged = deep_merge(merged, matrix_entry_override(inputs[input_id]))
    apply_unset(merged, inputs[input_id].get("$unset"))

    experiment_override = {key: value for key, value in experiment.items() if key not in EXPERIMENT_REF_KEYS}
    reject_profiling_override(experiment_override, f"experiments[{index - 1}]")
    merged = deep_merge(merged, experiment_override)
    apply_unset(merged, experiment.get("$unset"))
    merged["id"] = experiment_id
    merged["name"] = str(experiment.get("name") or experiment_id)
    attach_suite_metadata(
        merged,
        experiment_id=experiment_id,
        server_id=server_id,
        input_id=input_id,
    )
    return merged


def expand_suite(cfg: dict[str, Any]) -> list[dict[str, Any]]:
    """展开 suite 配置；普通单 run 配置原样返回。"""

    matrix = cfg.get("matrix")
    experiments = cfg.get("experiments")
    if experiments is None and matrix is None:
        return [cfg]

    if matrix is not None and not isinstance(matrix, dict):
        raise TypeError("matrix must be an object")
    if experiments is None:
        experiments = generated_matrix_experiments(matrix)
    if not isinstance(experiments, list) or not experiments:
        raise ValueError("experiments must be a non-empty list")

    common = {key: value for key, value in cfg.items() if key not in INTERNAL_SUITE_KEYS}
    expanded = []
    for index, experiment in enumerate(experiments, start=1):
        if not isinstance(experiment, dict):
            raise TypeError(f"experiments[{index - 1}] must be an object")
        if matrix is not None:
            merged = expand_matrix_experiment(common, matrix, experiment, index)
        else:
            reject_profiling_override(experiment, f"experiments[{index - 1}]")
            experiment_id = experiment_identity(experiment, index)
            merged = deep_merge(common, experiment)
            apply_unset(merged, experiment.get("$unset"))
            merged["id"] = experiment_id
            merged["name"] = str(experiment.get("name") or experiment_id)
            attach_suite_metadata(
                merged,
                experiment_id=experiment_id,
                server_id=None,
                input_id=None,
            )
        expanded.append(merged)
    return expanded


def filter_suite_experiments(
    experiments: list[tuple[int, dict[str, Any]]],
    selected_experiments: set[str],
    *,
    selected_inputs: set[str] | None = None,
    selected_servers: set[str] | None = None,
) -> list[tuple[int, dict[str, Any]]]:
    """根据 CLI/env 选择器过滤 suite 实验。"""

    selected_inputs = selected_inputs or set()
    selected_servers = selected_servers or set()
    available_inputs = sorted(
        {
            str((experiment.get("metadata") or {}).get("suite_input_id"))
            for _index, experiment in experiments
            if isinstance(experiment.get("metadata"), dict)
            and (experiment.get("metadata") or {}).get("suite_input_id")
        }
    )
    available_servers = sorted(
        {
            str((experiment.get("metadata") or {}).get("suite_server_id"))
            for _index, experiment in experiments
            if isinstance(experiment.get("metadata"), dict)
            and (experiment.get("metadata") or {}).get("suite_server_id")
        }
    )

    if not selected_experiments:
        selected = list(experiments)
    else:
        selected = []
        matched: set[str] = set()
        for index, experiment in experiments:
            selectors = experiment_selectors(experiment, index)
            overlap = selected_experiments & selectors
            if overlap:
                selected.append((index, experiment))
                matched.update(overlap)

        missing = sorted(selected_experiments - matched)
        if missing:
            available = ", ".join(str(item[1].get("id") or item[1].get("name") or item[0]) for item in experiments)
            raise ValueError(f"unknown experiment selector(s): {', '.join(missing)}; available: {available}")

    missing_inputs = selected_inputs - set(available_inputs)
    if missing_inputs:
        raise ValueError(
            f"unknown input selector(s): {', '.join(sorted(missing_inputs))}; "
            f"available inputs: {', '.join(available_inputs)}"
        )
    missing_servers = selected_servers - set(available_servers)
    if missing_servers:
        raise ValueError(
            f"unknown server selector(s): {', '.join(sorted(missing_servers))}; "
            f"available servers: {', '.join(available_servers)}"
        )

    def metadata_value(experiment: dict[str, Any], key: str) -> str:
        metadata = experiment.get("metadata") if isinstance(experiment.get("metadata"), dict) else {}
        value = metadata.get(key)
        return str(value) if isinstance(value, str) else ""

    filtered = [
        (index, experiment)
        for index, experiment in selected
        if (not selected_inputs or metadata_value(experiment, "suite_input_id") in selected_inputs)
        and (not selected_servers or metadata_value(experiment, "suite_server_id") in selected_servers)
    ]
    if not filtered:
        raise ValueError("no experiments matched the selected experiment/input/server combination")
    return filtered


def repo_relative_text(path: Path) -> str:
    """优先把 artifact 路径写成仓库相对形式。"""

    resolved = path.resolve()
    try:
        return str(resolved.relative_to(ROOT_DIR.resolve()))
    except ValueError:
        return str(resolved)


def single_run_artifact(run_dir: Path, pattern: str) -> Path:
    """查找 run 中唯一 artifact，避免 bundle 聚合静默选错文件。"""

    candidates = sorted(run_dir.glob(pattern))
    if len(candidates) != 1:
        raise ValueError(
            f"expected exactly one {pattern} under {run_dir}, found {len(candidates)}"
        )
    return candidates[0]


def build_forced_token_bundle(
    suite_dir: Path,
    run_dirs: list[Path],
    *,
    capture_config_path: Path,
) -> dict[str, Any]:
    """把 capture experiment 的 run-local plan 聚合成 suite-level bundle。"""

    plans_dir = suite_dir / "forced_token_plans"
    plans_dir.mkdir(parents=True, exist_ok=True)
    plans: dict[str, dict[str, Any]] = {}
    model_paths: set[str] = set()
    server_config_ids: set[str] = set()
    for run_dir in run_dirs:
        config = load_json(run_dir / "config.json")
        metadata = config.get("metadata") if isinstance(config.get("metadata"), dict) else {}
        input_id = str(metadata.get("suite_input_id") or "")
        server_config_id = str(metadata.get("suite_server_id") or "")
        if not input_id or not server_config_id:
            raise ValueError(f"capture run is missing suite input/server metadata: {run_dir}")
        if input_id in plans:
            raise ValueError(f"duplicate capture plan for input: {input_id}")

        source_plan = single_run_artifact(run_dir, "bench/**/forced_token_plan.json")
        workload_report = single_run_artifact(run_dir, "bench/**/workload_report.json")
        quality = forced_token_quality_from_workload_report(workload_report)
        if quality.get("mode") != "capture" or not quality.get("ready"):
            raise ValueError(
                f"capture forced-token contract is not ready for {input_id}: "
                f"{quality.get('errors', [])}"
            )
        plan = load_forced_token_plan(source_plan)
        if (
            quality.get("plan_workload_id") != input_id
            or quality.get("plan_workload_fingerprint") != plan.get("workload_fingerprint")
            or int(quality.get("request_count") or 0) != len(plan.get("requests") or [])
        ):
            raise ValueError(f"capture plan/report metadata mismatch for input: {input_id}")
        target_plan = plans_dir / f"{sanitize(input_id)}.json"
        shutil.copy2(source_plan, target_plan)
        target_sha256 = sha256_file(target_plan)
        if quality.get("plan_sha256") != target_sha256:
            raise ValueError(
                f"capture plan hash changed while aggregating {input_id}: "
                f"report={quality.get('plan_sha256')} copied={target_sha256}"
            )
        capture = plan.get("capture") if isinstance(plan.get("capture"), dict) else {}
        if capture.get("model_path"):
            model_paths.add(str(capture["model_path"]))
        server_config_ids.add(server_config_id)
        plans[input_id] = {
            "path": str(target_plan.relative_to(suite_dir)),
            "sha256": target_sha256,
            "workload_id": str(quality.get("plan_workload_id") or input_id),
            "workload_fingerprint": quality.get("plan_workload_fingerprint"),
            "request_count": int(quality.get("request_count") or 0),
            "workload_report": str(workload_report.relative_to(suite_dir)),
            "capture_run_dir": str(run_dir.relative_to(suite_dir)),
            "capture_run_id": capture.get("run_id") or run_dir.name,
            "capture_config_id": capture.get("config_id") or server_config_id,
        }

    if len(model_paths) != 1:
        raise ValueError(f"capture bundle requires exactly one model path, found: {sorted(model_paths)}")
    if len(server_config_ids) != 1:
        raise ValueError(
            f"capture bundle requires exactly one server config, found: {sorted(server_config_ids)}"
        )
    model_path = next(iter(model_paths))
    server_config_id = next(iter(server_config_ids))
    stable_identity = {
        "schema": FORCED_TOKEN_BUNDLE_SCHEMA,
        "capture_config": repo_relative_text(capture_config_path),
        "model_path": model_path,
        "server_config_id": server_config_id,
        "plans": plans,
    }
    payload = {
        "schema": FORCED_TOKEN_BUNDLE_SCHEMA,
        "bundle_id": sha256_json(stable_identity),
        "created_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "capture_suite_dir": repo_relative_text(suite_dir),
        "capture_config": repo_relative_text(capture_config_path),
        "model_path": model_path,
        "server_config_id": server_config_id,
        "plans": plans,
    }
    bundle_path = suite_dir / "forced_token_bundle.json"
    dump_json(bundle_path, payload)
    return forced_token_bundle_summary(bundle_path)


def run_profile_suite(
    cfg: dict[str, Any],
    dry_run: bool,
    selected_experiments: set[str] | None = None,
    selected_inputs: set[str] | None = None,
    selected_servers: set[str] | None = None,
    *,
    forced_token_bundle: Path | None = None,
    config_path: Path | None = None,
) -> list[Path]:
    """执行普通 run 或 suite，并写出 suite 级选择/结果文件。"""

    is_suite = "experiments" in cfg or "matrix" in cfg
    all_experiments = list(enumerate(expand_suite(cfg), start=1))
    experiments = filter_suite_experiments(
        all_experiments,
        selected_experiments or set(),
        selected_inputs=selected_inputs,
        selected_servers=selected_servers,
    )
    if len(experiments) == 1 and not is_suite:
        experiment = inject_forced_token_bundle_plan(experiments[0][1], forced_token_bundle)
        preflight_profile_config(experiment)
        return [run_profile(experiment, dry_run)]

    framework = cfg.get("framework", "sglang")
    suite_name = sanitize(str(cfg.get("name", f"{framework}-profile-suite")))
    suite_root = resolve_repo_path(cfg.get("run_root")) or ROOT_DIR / "data/profile_runs" / str(framework)
    suite_id = cfg.get("run_id") or f"{time.strftime('%Y%m%d_%H%M%S')}_{suite_name}"
    suite_dir = suite_root / sanitize(str(suite_id))
    run_dirs: list[Path] = []
    failures: list[dict[str, Any]] = []
    continue_on_error = bool(cfg.get("continue_on_error", False))
    prepared_experiments: list[tuple[int, int, str, dict[str, Any], dict[str, Any]]] = []
    for ordinal, (index, experiment) in enumerate(experiments, start=1):
        exp_name = sanitize(str(experiment.get("name", f"experiment-{index}")))
        exp_cfg = inject_forced_token_bundle_plan(experiment, forced_token_bundle)
        exp_cfg["run_root"] = str(suite_dir)
        exp_cfg["run_id"] = f"{index:02d}_{exp_name}"
        forced_token_contract = preflight_profile_config(exp_cfg)
        prepared_experiments.append((ordinal, index, exp_name, exp_cfg, forced_token_contract))

    suite_dir.mkdir(parents=True, exist_ok=True)
    dump_json(suite_dir / "suite_config.json", cfg)
    log(f"Suite dir: {suite_dir}")
    dump_json(
        suite_dir / "suite_selection.json",
        {
            "schema": "trace_sim.profile.suite_selection.v1",
            "suite_name": suite_name,
            "framework": framework,
            "profile_mode": suite_profile_mode(cfg),
            "metadata": cfg.get("metadata", {}) if isinstance(cfg.get("metadata"), dict) else {},
            "selected_selectors": sorted(selected_experiments or []),
            "selected_inputs": sorted(selected_inputs or []),
            "selected_servers": sorted(selected_servers or []),
            "forced_token_bundle": forced_token_bundle_summary(forced_token_bundle)
            if forced_token_bundle is not None
            else None,
            "available_experiments": [describe_suite_experiment(index, experiment) for index, experiment in all_experiments],
            "planned_experiments": [
                {
                    **describe_suite_experiment(index, exp_cfg),
                    "forced_token_contract": forced_token_contract,
                }
                for _ordinal, index, _exp_name, exp_cfg, forced_token_contract in prepared_experiments
            ],
        },
    )

    fatal_error: Exception | None = None
    attempted_count = 0
    for ordinal, index, exp_name, exp_cfg, forced_token_contract in prepared_experiments:
        attempted_count += 1
        log(f"Suite experiment {ordinal}/{len(experiments)} (#{index}): {exp_name}")
        try:
            run_dirs.append(run_profile(exp_cfg, dry_run))
        except Exception as exc:
            failures.append({"name": exp_name, "error": str(exc), "forced_token_contract": forced_token_contract})
            if not continue_on_error:
                fatal_error = exc
                break

    generated_bundle: dict[str, Any] | None = None
    bundle_error: str | None = None
    if suite_profile_mode(cfg) == "forced_token_capture" and not dry_run and not failures:
        try:
            if config_path is None:
                raise ValueError("capture suite requires its source config path")
            generated_bundle = build_forced_token_bundle(
                suite_dir,
                run_dirs,
                capture_config_path=config_path,
            )
            log(f"Forced token bundle: {generated_bundle.get('path')}")
        except Exception as exc:
            bundle_error = str(exc)
            failures.append({"name": "forced_token_bundle", "error": bundle_error})

    dump_json(
        suite_dir / "suite_result.json",
        {
            "schema": "trace_sim.profile.suite_result.v1",
            "suite_dir": str(suite_dir),
            "suite_name": suite_name,
            "framework": framework,
            "profile_mode": suite_profile_mode(cfg),
            "metadata": cfg.get("metadata", {}) if isinstance(cfg.get("metadata"), dict) else {},
            "dry_run": bool(dry_run),
            "selected_selectors": sorted(selected_experiments or []),
            "selected_inputs": sorted(selected_inputs or []),
            "selected_servers": sorted(selected_servers or []),
            "planned_count": len(prepared_experiments),
            "attempted_count": attempted_count,
            "completed_count": len(run_dirs),
            "failure_count": len(failures),
            "aborted_count": len(prepared_experiments) - attempted_count,
            "status": "failed" if failures else "completed",
            "runs": [str(path) for path in run_dirs],
            "failures": failures,
            "forced_token_contracts": summarize_suite_forced_token_contracts(
                [
                    forced_token_contract
                    for _ordinal, _index, _exp_name, _exp_cfg, forced_token_contract in prepared_experiments
                ]
            ),
            "forced_token_bundle": forced_token_bundle_summary(forced_token_bundle)
            if forced_token_bundle is not None
            else None,
            "generated_forced_token_bundle": generated_bundle,
            "selected_experiments": [
                {
                    **describe_suite_experiment(index, exp_cfg),
                    "forced_token_contract": forced_token_contract,
                }
                for _ordinal, index, _exp_name, exp_cfg, forced_token_contract in prepared_experiments
            ],
        },
    )
    if bundle_error:
        raise ValueError(f"forced token bundle aggregation failed: {bundle_error}")
    if fatal_error is not None:
        raise RuntimeError(f"profile suite failed: {failures[-1]['name']}: {fatal_error}") from fatal_error
    return run_dirs


def suite_profile_mode(cfg: dict[str, Any]) -> str | None:
    """读取 suite metadata 中的 profile mode。"""

    metadata = cfg.get("metadata") if isinstance(cfg.get("metadata"), dict) else {}
    value = metadata.get("profile_mode")
    return str(value) if isinstance(value, str) and value else None


def summarize_suite_forced_token_contracts(contracts: list[dict[str, Any]]) -> dict[str, Any]:
    """汇总 suite 中 forced-token preflight 合同，便于顶层审计。"""

    modes = sorted({str(contract.get("mode") or "none") for contract in contracts})
    errors = sorted(
        {
            str(error)
            for contract in contracts
            for error in contract.get("errors", [])
        }
    )
    plan_hashes = sorted(
        {
            str(plan.get("sha256"))
            for contract in contracts
            for plan in [contract.get("plan")]
            if isinstance(plan, dict) and plan.get("sha256")
        }
    )
    bundle_hashes = sorted(
        {
            str(bundle.get("sha256"))
            for contract in contracts
            for bundle in [contract.get("bundle")]
            if isinstance(bundle, dict) and bundle.get("sha256")
        }
    )
    bundle_ids = sorted(
        {
            str(bundle.get("bundle_id"))
            for contract in contracts
            for bundle in [contract.get("bundle")]
            if isinstance(bundle, dict) and bundle.get("bundle_id")
        }
    )
    workloads = sorted(
        {
            str(contract.get("workload_id"))
            for contract in contracts
            if contract.get("workload_id")
        }
    )
    return {
        "mode_count": {mode: sum(1 for contract in contracts if str(contract.get("mode") or "none") == mode) for mode in modes},
        "errors": errors,
        "ready": not errors,
        "workload_ids": workloads,
        "plan_sha256_count": len(plan_hashes),
        "plan_sha256_values": plan_hashes,
        "bundle_sha256_count": len(bundle_hashes),
        "bundle_sha256_values": bundle_hashes,
        "bundle_ids": bundle_ids,
    }


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """解析容器内 runner CLI 参数。"""

    parser = argparse.ArgumentParser(description="Run SGLang profiling experiments.")
    parser.add_argument("--config", required=True, help="JSON profile config path")
    parser.add_argument("--dry-run", action="store_true", help="expand config and manifest without starting the server")
    parser.add_argument("--experiment", action="append", default=[], help="run one experiment id/name; may be repeated")
    parser.add_argument("--experiments", action="append", default=[], help="comma-separated experiment ids/names to run")
    parser.add_argument("--input", action="append", default=[], help="run one suite input id; may be repeated")
    parser.add_argument("--inputs", action="append", default=[], help="comma-separated suite input ids to run")
    parser.add_argument("--server", action="append", default=[], help="run one suite server id; may be repeated")
    parser.add_argument("--servers", action="append", default=[], help="comma-separated suite server ids to run")
    parser.add_argument(
        "--forced-token-bundle",
        help="Explicit forced_token_bundle.json required by forced-token replay suites.",
    )
    parser.add_argument("--list-experiments", action="store_true", help="print expanded experiment ids without running them")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    """CLI 入口：执行 profiling run/suite 或列出展开后的实验。"""

    args = parse_args(argv)
    config_path = resolve_repo_path(args.config)
    if config_path is None or not config_path.is_file():
        raise FileNotFoundError(f"missing config: {args.config}")
    cfg = load_json(config_path)
    selected_experiments = parse_experiment_selection(
        [*args.experiment, *args.experiments],
        os.environ.get(PROFILE_EXPERIMENTS_ENV),
    )
    selected_inputs = parse_experiment_selection(
        [*args.input, *args.inputs],
        os.environ.get(PROFILE_INPUTS_ENV),
    )
    selected_servers = parse_experiment_selection(
        [*args.server, *args.servers],
        os.environ.get(PROFILE_SERVERS_ENV),
    )
    forced_token_bundle = resolve_repo_path(
        args.forced_token_bundle or os.environ.get(PROFILE_FORCED_TOKEN_BUNDLE_ENV)
    )
    if args.list_experiments:
        experiments = filter_suite_experiments(
            list(enumerate(expand_suite(cfg), start=1)),
            selected_experiments,
            selected_inputs=selected_inputs,
            selected_servers=selected_servers,
        )
        for index, experiment in experiments:
            exp_id = experiment.get("id") or experiment_identity(experiment, index)
            exp_name = experiment.get("name") or exp_id
            metadata = experiment.get("metadata") if isinstance(experiment.get("metadata"), dict) else {}
            print(
                f"{index:02d}\t{exp_id}\t{exp_name}\t"
                f"server={metadata.get('suite_server_id')}\tinput={metadata.get('suite_input_id')}"
            )
        return 0

    run_dirs = run_profile_suite(
        cfg,
        args.dry_run,
        selected_experiments,
        selected_inputs,
        selected_servers,
        forced_token_bundle=forced_token_bundle,
        config_path=config_path,
    )
    for run_dir in run_dirs:
        print(run_dir)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (FileNotFoundError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        raise SystemExit(2)
    except KeyboardInterrupt:
        raise SystemExit(130)
