"""HiCache forced-token profiling workflow helpers.

该模块连接 profiling suite 与 HiCache phased workload 的 forced-token 契约：

- 解析 workload 命令中的 forced-token 参数；
- 在 replay suite 中按 input 注入 bundle 内的 plan；
- 在 capture suite 结束后聚合 suite-level bundle。

这里不启动 server，也不写 profile manifest；这些仍属于 profiling runner。
"""

from __future__ import annotations

import argparse
import copy
import json
import shutil
import sys
import time
from pathlib import Path
from typing import Any

from ..common.commands import command_from_config, command_tokens
from ..common.io import load_json, write_json
from ..common.naming import sanitize
from ..common.paths import ROOT_DIR, resolve_repo_path
from ..contracts.forced_token import (
    FORCED_TOKEN_BUNDLE_SCHEMA,
    FORCED_TOKEN_ERROR_BUNDLE_PROVENANCE,
    FORCED_TOKEN_ERROR_CAPTURE_OVERWRITE,
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


BENCH_SCRIPT_ROOT = ROOT_DIR / "scripts/bench"
if str(BENCH_SCRIPT_ROOT) not in sys.path:
    sys.path.insert(0, str(BENCH_SCRIPT_ROOT))

from hicache_phased_workload import (  # noqa: E402
    build_arg_parser as build_hicache_workload_arg_parser,
    build_plan as build_hicache_workload_plan,
    logical_request_id as hicache_logical_request_id,
    workload_args_digest as hicache_workload_args_digest,
    workload_id as hicache_workload_id,
)


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
        hicache_logical_request_id(workload_args, item, sequence_id) for sequence_id, item in enumerate(workload_plan)
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


def preflight_forced_token_contract(
    bench_command: list[str] | str | None,
    metadata: dict[str, Any],
    *,
    experiment_id: str,
) -> dict[str, Any]:
    """校验单个 expanded profile config 的 forced-token contract。"""

    bundle = metadata.get("forced_token_bundle")
    bundle_provenance = bundle if isinstance(bundle, dict) else None
    report = forced_token_contract_report(bench_command, bundle_provenance)
    forced_errors = [str(error) for error in report.get("errors", [])]
    if forced_errors:
        raise ValueError(f"forced token preflight failed for {experiment_id}: {', '.join(forced_errors)}")
    return report


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
        raise ValueError(f"expected exactly one {pattern} under {run_dir}, found {len(candidates)}")
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
            raise ValueError(f"capture forced-token contract is not ready for {input_id}: {quality.get('errors', [])}")
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
        raise ValueError(f"capture bundle requires exactly one server config, found: {sorted(server_config_ids)}")
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
    write_json(bundle_path, payload)
    return forced_token_bundle_summary(bundle_path)
