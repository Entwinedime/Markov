"""Forced-token capture/replay wiring for profiling suites."""

from __future__ import annotations

import copy
import json
import shutil
from pathlib import Path
from typing import Any

from ..common.commands import command_from_config, command_tokens
from ..common.io import load_json, write_json
from ..common.naming import sanitize
from ..common.paths import resolve_repo_path
from ..contracts.forced_token.bundle import resolve_forced_token_bundle_plan
from ..contracts.forced_token.constants import (
    FORCED_TOKEN_ERROR_CAPTURE_OVERWRITE,
    FORCED_TOKEN_ERROR_PLAN_MISSING,
)
from ..contracts.forced_token.plan import (
    forced_token_plan_summary,
    load_forced_token_plan,
    validate_plan_contract,
)
from ..contracts.forced_token.quality import forced_token_quality_from_workload_report


def _driver_argv(tokens: list[str], script: str) -> list[str] | None:
    for index, token in enumerate(tokens):
        if token.endswith(script):
            return tokens[index + 1 :]
    return None


def _template_workload_args(command: list[str] | str | None) -> dict[str, str] | None:
    argv = _driver_argv(command_tokens(command), "hicache_template_workload.py")
    if argv is None:
        return None
    result: dict[str, str] = {"forced_token_mode": "none"}
    for option, field in (
        ("--template", "template"),
        ("--output-dir", "output_dir"),
        ("--forced-token-mode", "forced_token_mode"),
        ("--forced-token-plan", "forced_token_plan"),
    ):
        if option in argv:
            index = argv.index(option)
            if index + 1 >= len(argv):
                raise ValueError(f"{option} is missing its value")
            result[field] = argv[index + 1]
    if "template" not in result or "output_dir" not in result:
        raise ValueError("template workload requires --template and --output-dir")
    return result


def forced_token_plan_path_from_args(args: dict[str, str]) -> Path | None:
    if args.get("forced_token_plan"):
        return resolve_repo_path(args["forced_token_plan"])
    if args["forced_token_mode"] == "capture":
        return Path(args["output_dir"]) / "forced_token_plan.json"
    return None


def forced_token_contract_report(
    bench_command: list[str] | str | None,
    _bundle_metadata: dict[str, Any] | None = None,
) -> dict[str, Any]:
    template = _template_workload_args(bench_command)
    if template is None:
        return {"enabled": False, "mode": "none", "errors": []}
    mode = template["forced_token_mode"]
    plan_path = forced_token_plan_path_from_args(template)
    template_path = resolve_repo_path(template["template"])
    raw = load_json(template_path) if template_path else None
    if not isinstance(raw, dict) or not raw.get("id"):
        raise ValueError(f"invalid workload template: {template_path}")
    workload_id = str(raw["id"])
    report = {
        "enabled": mode != "none",
        "mode": mode,
        "errors": [],
        "workload_id": workload_id,
        "plan_path": str(plan_path) if plan_path else None,
        "plan": forced_token_plan_summary(plan_path).to_dict(),
    }
    if mode == "none":
        return report
    if plan_path is None:
        report["errors"] = [FORCED_TOKEN_ERROR_PLAN_MISSING]
    elif mode == "capture":
        if plan_path.exists():
            report["errors"] = [FORCED_TOKEN_ERROR_CAPTURE_OVERWRITE]
    elif mode == "replay":
        try:
            plan = load_forced_token_plan(plan_path)
            report["errors"] = validate_plan_contract(
                plan,
                workload_id=workload_id,
                expected_request_ids=None,
            )
        except (OSError, json.JSONDecodeError, ValueError) as error:
            report["errors"] = [f"forced_token_plan_invalid:{error}"]
    else:
        report["errors"] = [f"unsupported_forced_token_mode:{mode}"]
    return report


def forced_token_mode_from_config(cfg: dict[str, Any]) -> str:
    bench = cfg.get("bench") if isinstance(cfg.get("bench"), dict) else {}
    tokens = command_tokens(command_from_config(bench.get("command"))) if "command" in bench else []
    if "--forced-token-mode" not in tokens:
        return "none"
    index = tokens.index("--forced-token-mode")
    if index + 1 >= len(tokens):
        raise ValueError("--forced-token-mode is missing its value")
    return str(tokens[index + 1])


def inject_forced_token_bundle_plan(cfg: dict[str, Any], bundle_path: Path | None) -> dict[str, Any]:
    result = copy.deepcopy(cfg)
    mode = forced_token_mode_from_config(result)
    if mode != "replay":
        if bundle_path is not None:
            raise ValueError("--forced-token-bundle can only be used with replay experiments")
        return result
    if bundle_path is None:
        raise ValueError("forced-token replay requires --forced-token-bundle")
    metadata = result.get("metadata") if isinstance(result.get("metadata"), dict) else {}
    input_id = str(metadata.get("suite_input_id") or "")
    if not input_id:
        raise ValueError("forced-token replay experiment is missing metadata.suite_input_id")
    plan_path = resolve_forced_token_bundle_plan(bundle_path, input_id).plan_path
    bench = result.get("bench") if isinstance(result.get("bench"), dict) else {}
    tokens = command_tokens(command_from_config(bench.get("command"))) if "command" in bench else []
    if "--forced-token-plan" not in tokens:
        raise ValueError("forced-token replay requires --forced-token-plan {forced_token_plan}")
    index = tokens.index("--forced-token-plan")
    if index + 1 >= len(tokens) or tokens[index + 1] != "{forced_token_plan}":
        raise ValueError("forced-token replay plan argument must be {forced_token_plan}")
    tokens[index + 1] = plan_path
    result.setdefault("bench", {})["command"] = tokens
    return result


def preflight_forced_token_contract(
    bench_command: list[str] | str | None,
    metadata: dict[str, Any],
    *,
    experiment_id: str,
) -> dict[str, Any]:
    report = forced_token_contract_report(bench_command, metadata)
    errors = [str(error) for error in report.get("errors", [])]
    if errors:
        raise ValueError(f"forced token preflight failed for {experiment_id}: {', '.join(errors)}")
    return report


def _single_run_artifact(run_dir: Path, pattern: str) -> Path:
    candidates = sorted(run_dir.glob(pattern))
    if len(candidates) != 1:
        raise ValueError(f"expected exactly one {pattern} under {run_dir}, found {len(candidates)}")
    return candidates[0]


def build_forced_token_bundle(suite_dir: Path, run_dirs: list[Path]) -> dict[str, Any]:
    """Copy successful capture plans and write the input-to-path index."""

    plans_dir = suite_dir / "forced_token_plans"
    plans_dir.mkdir(parents=True, exist_ok=True)
    plans: dict[str, dict[str, str]] = {}
    for run_dir in run_dirs:
        config = load_json(run_dir / "config.json")
        metadata = config.get("metadata") if isinstance(config.get("metadata"), dict) else {}
        input_id = str(metadata.get("suite_input_id") or "")
        if not input_id or input_id in plans:
            raise ValueError(f"missing or duplicate capture input id: {input_id}")
        source_plan = _single_run_artifact(run_dir, "bench/**/forced_token_plan.json")
        report = _single_run_artifact(run_dir, "bench/**/workload_report.json")
        quality = forced_token_quality_from_workload_report(report)
        plan = load_forced_token_plan(source_plan)
        if quality.get("mode") != "capture" or not quality.get("ready") or plan.get("workload_id") != input_id:
            raise ValueError(f"capture forced-token plan is not ready for {input_id}: {quality.get('errors', [])}")
        target = plans_dir / f"{sanitize(input_id)}.json"
        shutil.copy2(source_plan, target)
        plans[input_id] = {"path": str(target.relative_to(suite_dir))}
    bundle_path = suite_dir / "forced_token_bundle.json"
    write_json(bundle_path, {"plans": plans})
    return {"path": str(bundle_path), "input_ids": sorted(plans), "plan_count": len(plans), "ready": True}
