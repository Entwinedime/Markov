"""Forced-token capture/replay wiring for profiling suites."""

from __future__ import annotations

import copy
import json
import shutil
from pathlib import Path
from typing import Any

from ..common.commands import command_from_config, command_tokens, replace_command_option
from ..common.io import load_json, write_json
from ..common.manifest import profile_labels, workload_report_path
from ..common.naming import sanitize
from ..common.paths import require_repo_path, resolve_repo_path
from ..contracts.forced_token.bundle import resolve_forced_token_bundle_plan
from ..contracts.forced_token.constants import (
    FORCED_TOKEN_ERROR_CAPTURE_OVERWRITE,
    FORCED_TOKEN_ERROR_PLAN_MISSING,
)
from ..contracts.forced_token.plan import (
    load_forced_token_plan,
    validate_plan_contract,
)
from ..contracts.forced_token.quality import forced_token_quality_from_report
from ..workload_template.cli import parse_workload_command


def preflight_forced_token_contract(
    bench_command: list[str] | str | None,
    *,
    experiment_id: str,
) -> None:
    """Reject invalid replay plans or capture overwrite before starting inference."""

    template = parse_workload_command(bench_command)
    if template is None:
        return

    mode = template.forced_token_mode
    plan_path = resolve_repo_path(template.forced_token_plan)
    if plan_path is None and mode == "capture":
        plan_path = Path(template.output_dir) / "forced_token_plan.json"
    template_path = resolve_repo_path(template.template)
    raw = load_json(template_path) if template_path else None
    if not isinstance(raw, dict) or not raw.get("id"):
        raise ValueError(f"invalid workload template: {template_path}")
    workload_id = str(raw["id"])
    if mode == "none":
        return

    errors: list[str] = []
    if plan_path is None:
        errors = [FORCED_TOKEN_ERROR_PLAN_MISSING]
    elif mode == "capture":
        if plan_path.exists():
            errors = [FORCED_TOKEN_ERROR_CAPTURE_OVERWRITE]
    elif mode == "replay":
        try:
            plan = load_forced_token_plan(plan_path)
            errors = validate_plan_contract(
                plan,
                workload_id=workload_id,
                expected_request_ids=None,
            )
        except (OSError, json.JSONDecodeError, ValueError) as error:
            errors = [f"forced_token_plan_invalid:{error}"]

    if errors:
        raise ValueError(f"forced token preflight failed for {experiment_id}: {', '.join(errors)}")


def inject_forced_token_bundle_plan(cfg: dict[str, Any], bundle_path: Path | None) -> dict[str, Any]:
    result = copy.deepcopy(cfg)
    bench = result.get("bench", {})
    tokens = command_tokens(command_from_config(bench["command"])) if "command" in bench else []
    args = parse_workload_command(tokens)
    mode = args.forced_token_mode if args is not None else "none"
    if mode != "replay":
        if bundle_path is not None:
            raise ValueError("--forced-token-bundle can only be used with replay experiments")
        return result
    if bundle_path is None:
        if not args.forced_token_plan or args.forced_token_plan == "{forced_token_plan}":
            raise ValueError("forced-token replay requires a plan path or --forced-token-bundle")
        return result

    _, input_id = profile_labels(result)
    if not input_id:
        raise ValueError("forced-token replay requires metadata.workload_id (or a suite input label)")
    plan_path = resolve_forced_token_bundle_plan(bundle_path, input_id)
    if args.forced_token_plan != "{forced_token_plan}":
        raise ValueError("forced-token replay plan argument must be {forced_token_plan}")
    replace_command_option(tokens, "--forced-token-plan", str(plan_path))
    bench["command"] = tokens
    return result


def build_forced_token_bundle(suite_dir: Path, run_dirs: list[Path]) -> dict[str, Any]:
    """Copy successful capture plans and write the input-to-path index."""

    plans_dir = suite_dir / "forced_token_plans"
    plans_dir.mkdir(parents=True, exist_ok=True)
    plans: dict[str, dict[str, str]] = {}
    for run_dir in run_dirs:
        manifest = load_json(run_dir / "profile_manifest.json")
        config = load_json(require_repo_path(manifest["config_path"]))
        _, input_id = profile_labels(config)
        if not input_id or input_id in plans:
            raise ValueError(f"missing or duplicate capture input id: {input_id}")
        report = load_json(workload_report_path(manifest))
        quality = forced_token_quality_from_report(report)
        source_plan = require_repo_path(report["forced_token"]["plan_path"])
        plan = load_forced_token_plan(source_plan)
        if quality.get("mode") != "capture" or not quality.get("ready") or plan.get("workload_id") != input_id:
            raise ValueError(f"capture forced-token plan is not ready for {input_id}: {quality.get('errors', [])}")
        target = plans_dir / f"{sanitize(input_id)}.json"
        shutil.copy2(source_plan, target)
        plans[input_id] = {"path": str(target.relative_to(suite_dir))}
    bundle_path = suite_dir / "forced_token_bundle.json"
    write_json(bundle_path, {"plans": plans})
    return {"path": str(bundle_path), "input_ids": sorted(plans), "plan_count": len(plans), "ready": True}
