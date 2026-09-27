"""Run one deterministic JSON HiCache workload in capture or replay mode."""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path


from ..common.commands import command_tokens
from ..common.io import write_json
from ..common.paths import ROOT_DIR
from . import (
    TemplateValidationError,
    expand_template,
    load_config_specs,
    load_template,
    load_tokenizer,
)
from .executor import (
    WorkloadExecutionError,
    execute_workload,
)


DEFAULT_CONFIG_SPECS = ROOT_DIR / "configs/workloads/hicache_manual/configs.json"


def build_arg_parser() -> argparse.ArgumentParser:
    """Build the reusable command parser for runner preflight and execution."""

    parser = argparse.ArgumentParser(
        description="Compile and strictly serially execute a JSON HiCache manual workload.", allow_abbrev=False
    )
    parser.add_argument("--template", required=True, help="Path to one JSON workload template.")
    parser.add_argument(
        "--config-specs",
        default=str(DEFAULT_CONFIG_SPECS),
        help="Resolved-target config contract JSON.",
    )
    parser.add_argument("--config-id", help="Required resolved config id for a HiCache replay cell.")
    parser.add_argument("--tokenizer-path", default="/models/Qwen3-32B")
    parser.add_argument("--base-url", default="http://127.0.0.1:30000")
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--timeout-sec", type=float, default=600.0)
    parser.add_argument("--forced-token-mode", choices=("none", "capture", "replay"), default="none")
    parser.add_argument("--forced-token-plan")
    parser.add_argument(
        "--require-hicache-state",
        action="store_true",
        help="Enforce startup, barrier and checkpoint gates while recording real output tokens.",
    )
    parser.add_argument("--diagnostic-api-key", default=os.environ.get("TRACE_SIM_HICACHE_DIAGNOSTIC_API_KEY"))
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Compile/token-validate only; never contact a server or NPU.",
    )
    return parser


def parse_workload_command(command: list[str] | str | None) -> argparse.Namespace | None:
    """Parse this driver's options; other workload drivers are outside this contract."""

    tokens = command_tokens(command)
    for index, token in enumerate(tokens):
        if Path(token).name == "hicache_template_workload.py":
            return build_arg_parser().parse_args(tokens[index + 1 :])
    return None


def main() -> int:
    """Compile first, then execute the immutable sequence only when all gates pass."""

    parser = build_arg_parser()
    args = parser.parse_args()
    template_path = Path(args.template)
    config_specs_path = Path(args.config_specs)
    output_dir = Path(args.output_dir)
    try:
        template = load_template(template_path)
        configs = load_config_specs(config_specs_path) if args.config_id else {}
        tokenizer = (
            load_tokenizer(str(args.tokenizer_path))
            if any("prompt_token_ids" not in request for request in template.data["request_defs"].values())
            else None
        )
        plan = expand_template(template, tokenizer)
    except (OSError, RuntimeError, TemplateValidationError) as error:
        parser.error(str(error))

    if args.dry_run:
        output_dir.mkdir(parents=True, exist_ok=True)
        summary = {
            "status": "dry_run_passed",
            "workload_id": plan.template.workload_id,
            "request_count": len(plan.requests),
            "formal_window": {
                "start_step": plan.formal_start_step,
                "end_step": plan.formal_end_step,
            },
        }
        write_json(output_dir / "workload_report.json", summary)
        print(json.dumps(summary, ensure_ascii=False, sort_keys=True), flush=True)
        return 0

    config = None
    if args.config_id:
        config = configs.get(str(args.config_id))
        if config is None:
            parser.error(f"unknown config id: {args.config_id}")
    require_diagnostic = args.forced_token_mode == "replay" or args.require_hicache_state
    if require_diagnostic and config is None:
        parser.error("HiCache state checks require --config-id")
    forced_plan_path = Path(args.forced_token_plan) if args.forced_token_plan else None
    try:
        execute_workload(
            plan,
            base_url=str(args.base_url),
            output_dir=output_dir,
            mode=str(args.forced_token_mode),
            forced_token_plan_path=forced_plan_path,
            config=config,
            timeout_sec=float(args.timeout_sec),
            diagnostic_api_key=args.diagnostic_api_key,
            require_diagnostic=require_diagnostic,
        )
    except (OSError, TemplateValidationError, WorkloadExecutionError) as error:
        print(f"workload_failed:{error}", file=sys.stderr, flush=True)
        return 1
    return 0
