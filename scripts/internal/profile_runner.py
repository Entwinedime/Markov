#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any


ROOT_DIR = Path(__file__).resolve().parents[2]
INTERNAL_KEYS = {"experiments", "continue_on_error", "$unset"}


def log(message: str) -> None:
    timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
    print(f"[{timestamp}] {message}", flush=True)


def sanitize(value: str) -> str:
    value = re.sub(r"[^A-Za-z0-9_.-]+", "_", value.strip())
    return value.strip("._-") or "profile"


def load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


def dump_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as f:
        json.dump(value, f, indent=2, ensure_ascii=False)
        f.write("\n")


def deep_merge(base: dict[str, Any], override: dict[str, Any]) -> dict[str, Any]:
    merged = dict(base)
    for key, value in override.items():
        if key == "$unset":
            continue
        if (
            key in merged
            and isinstance(merged[key], dict)
            and isinstance(value, dict)
        ):
            merged[key] = deep_merge(merged[key], value)
        else:
            merged[key] = value
    return merged


def delete_path(value: dict[str, Any], path: str) -> None:
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


def build_bench_command(bench: dict[str, Any], run_dir: Path) -> list[str] | str | None:
    if not bench:
        return None

    if "command" in bench:
        return command_from_config(bench["command"])

    kind = bench.get("kind", "sglang.bench_serving")
    if kind != "sglang.bench_serving":
        raise ValueError(f"unknown bench kind: {kind}")

    bench_dir = run_dir / "bench"
    output_file = bench.get("output_file") or str(bench_dir / "bench.jsonl")
    args = dict(bench.get("args", {}))
    args.setdefault("output_file", output_file)

    command = ["python3", "-m", "sglang.bench_serving"]
    for key, value in args.items():
        append_cli_arg(command, key, value)
    return command


def parse_model_path(server_command: list[str] | str) -> str | None:
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
    run_dir: Path,
) -> tuple[Path | None, Path | None]:
    overrides = cfg.get("model_config_overrides") or {}
    if not overrides:
        return None, None
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

    backup_path = run_dir / "_config_backup" / sanitize(model_path.name) / "config.json"
    backup_path.parent.mkdir(parents=True, exist_ok=True)
    backup_path.write_bytes(config_path.read_bytes())

    data = load_json(config_path)
    data.update(overrides)
    dump_json(config_path, data)
    return config_path, backup_path


def restore_model_config(config_path: Path | None, backup_path: Path | None) -> None:
    if config_path and backup_path and backup_path.is_file():
        config_path.write_bytes(backup_path.read_bytes())


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
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_file = log_path.open("wb")
    shell = isinstance(command, str)
    return subprocess.Popen(
        command,
        cwd=ROOT_DIR,
        env=env,
        stdout=log_file,
        stderr=subprocess.STDOUT,
        shell=shell,
        preexec_fn=os.setsid,
    )


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


def build_profile_body(profile: dict[str, Any], run_dir: Path) -> dict[str, Any]:
    output_dir = resolve_run_path(profile.get("output_dir", "trace/torch"), run_dir)
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


def run_profile(cfg: dict[str, Any], dry_run: bool) -> Path:
    framework = cfg.get("framework", "sglang")
    if framework != "sglang":
        raise ValueError("scripts/internal/profile_runner.py currently supports framework=sglang")

    name = sanitize(cfg.get("name", "sglang-profile"))
    run_root = resolve_repo_path(cfg.get("run_root")) or ROOT_DIR / "data/profile_runs/sglang"
    run_id = cfg.get("run_id") or f"{time.strftime('%Y%m%d_%H%M%S')}_{name}"
    run_dir = run_root / sanitize(run_id)
    log_dir = run_dir / "logs"
    trace_dir = run_dir / "trace"
    torch_trace_dir = trace_dir / "torch"
    bench_dir = run_dir / "bench"
    for path in (log_dir, trace_dir, torch_trace_dir, bench_dir):
        path.mkdir(parents=True, exist_ok=True)

    server = cfg.get("server", {})
    server_command = command_from_config(server["command"])
    bench_command = build_bench_command(cfg.get("bench", {}), run_dir)
    dump_json(run_dir / "config.json", cfg)
    (run_dir / "server_cmd.txt").write_text(command_to_text(server_command) + "\n")
    if bench_command is not None:
        (run_dir / "bench_cmd.txt").write_text(command_to_text(bench_command) + "\n")

    log(f"Run dir: {run_dir}")
    if dry_run:
        return run_dir

    env = os.environ.copy()
    for key, value in cfg.get("env", {}).items():
        env[str(key)] = str(value)
    env.setdefault("HOOK_ASCENDCL_SO_PATH", "/usr/local/Ascend/ascend-toolkit/latest/lib64/libascendcl.so")
    env.setdefault("SGLANG_SET_CPU_AFFINITY", "1")
    env.setdefault("PYTORCH_NPU_ALLOC_CONF", "expandable_segments:True")
    env.setdefault("STREAMS_PER_DEVICE", "32")
    env.setdefault("HCCL_BUFFSIZE", "1536")
    env.setdefault("HCCL_OP_EXPANSION_MODE", "AIV")
    env["SGLANG_TORCH_PROFILER_DIR"] = str(torch_trace_dir)

    hook = cfg.get("hook", {})
    if hook.get("enabled", True):
        hook_lib = resolve_repo_path(
            hook.get("library", "build/docker/sglang/lib/libhook.so")
        )
        if hook_lib is None or not hook_lib.is_file():
            raise FileNotFoundError(
                f"missing hook library: {hook_lib}. Build it with scripts/build.sh sglang --hook-only."
            )
        env["LD_PRELOAD"] = str(hook_lib)
        env["HOOK_TRACE_OUTPUT"] = str(resolve_run_path(hook.get("trace_output", "trace/cpu_trace"), run_dir))

    ready_url = server.get("ready_url", "http://127.0.0.1:30000/get_model_info")
    ready_timeout_sec = int(server.get("ready_timeout_sec", 1800))
    api_base = cfg.get("profile", {}).get("api_base_url") or api_base_from_ready_url(ready_url)

    config_path = None
    backup_path = None
    server_process: subprocess.Popen[Any] | None = None
    try:
        config_path, backup_path = apply_model_config_overrides(cfg, server_command, run_dir)

        log("Starting SGLang server.")
        server_process = start_process(server_command, log_dir / "server.log", env)
        wait_for_ready(server_process, ready_url, ready_timeout_sec)
        log("Server is ready.")

        profile = cfg.get("profile", {})
        if profile.get("enabled", True):
            body = build_profile_body(profile, run_dir)
            dump_json(run_dir / "profile_start_body.json", body)
            log("Starting SGLang profiler via /start_profile.")
            response = post_json(api_base.rstrip("/") + "/start_profile", body)
            dump_json(run_dir / "profile_start_response.json", response)

        if bench_command is not None:
            log("Running workload.")
            bench_env = env.copy()
            bench_env.pop("LD_PRELOAD", None)
            bench_proc = start_process(bench_command, log_dir / "bench.log", bench_env)
            bench_code = bench_proc.wait()
            if bench_code != 0:
                raise RuntimeError(f"bench command failed, code={bench_code}")

        if profile.get("enabled", True) and profile.get("stop_after_workload", True):
            log("Stopping SGLang profiler via /stop_profile.")
            try:
                response = post_json(api_base.rstrip("/") + "/stop_profile", None)
            except Exception as exc:
                if profile.get("strict_stop", False):
                    raise
                response = {"warning": str(exc)}
            dump_json(run_dir / "profile_stop_response.json", response)

    finally:
        stop_process(server_process)
        restore_model_config(config_path, backup_path)

    log("Profile run completed.")
    return run_dir


def expand_suite(cfg: dict[str, Any]) -> list[dict[str, Any]]:
    experiments = cfg.get("experiments")
    if experiments is None:
        return [cfg]
    if not isinstance(experiments, list) or not experiments:
        raise ValueError("experiments must be a non-empty list")

    common = {key: value for key, value in cfg.items() if key not in INTERNAL_KEYS}
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
    suite_name = sanitize(cfg.get("name", f"{framework}-profile-suite"))
    suite_root = resolve_repo_path(cfg.get("run_root")) or ROOT_DIR / "data/profile_runs" / framework
    suite_id = cfg.get("run_id") or f"{time.strftime('%Y%m%d_%H%M%S')}_{suite_name}"
    suite_dir = suite_root / sanitize(suite_id)
    suite_dir.mkdir(parents=True, exist_ok=True)
    dump_json(suite_dir / "suite_config.json", cfg)

    log(f"Suite dir: {suite_dir}")
    run_dirs: list[Path] = []
    failures = []
    continue_on_error = bool(cfg.get("continue_on_error", False))
    for index, experiment in enumerate(experiments, start=1):
        exp_name = sanitize(experiment.get("name", f"experiment-{index}"))
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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", required=True, help="JSON profile config path")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    config_path = resolve_repo_path(args.config)
    if config_path is None or not config_path.is_file():
        raise FileNotFoundError(f"missing config: {args.config}")
    cfg = load_json(config_path)
    run_dirs = run_profile_suite(cfg, args.dry_run)
    for run_dir in run_dirs:
        print(run_dir)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
