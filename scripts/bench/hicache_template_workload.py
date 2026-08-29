#!/usr/bin/env python3
"""Run one deterministic JSON HiCache workload in capture or replay mode."""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
INTERNAL_DIR = SCRIPT_DIR.parent / "internal"
if str(INTERNAL_DIR) not in sys.path:
    sys.path.insert(0, str(INTERNAL_DIR))

from markov_internal.workload_template import (  # noqa: E402
    TemplateValidationError,
    expand_template,
    load_config_specs,
    load_template,
    load_tokenizer,
)
from markov_internal.workload_template.executor import (  # noqa: E402
    WorkloadExecutionError,
    execute_workload,
)
from markov_internal.workload_template.report import write_outputs  # noqa: E402


ROOT_DIR = SCRIPT_DIR.parents[1]
DEFAULT_CONFIG_SPECS = ROOT_DIR / "configs/workloads/hicache_manual/configs.json"


def build_arg_parser() -> argparse.ArgumentParser:
    """Build the reusable command parser for runner preflight and execution."""

    parser = argparse.ArgumentParser(
        description="Compile and strictly serially execute a JSON HiCache manual workload."
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
    parser.add_argument("--diagnostic-api-key", default=os.environ.get("TRACE_SIM_HICACHE_DIAGNOSTIC_API_KEY"))
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Compile/token-validate only; never contact a server or NPU.",
    )
    return parser


def main() -> int:
    """Compile first, then execute the immutable sequence only when all gates pass."""

    parser = build_arg_parser()
    args = parser.parse_args()
    template_path = Path(args.template)
    config_specs_path = Path(args.config_specs)
    output_dir = Path(args.output_dir)
    try:
        template = load_template(template_path)
        configs = load_config_specs(config_specs_path)
        tokenizer = load_tokenizer(str(args.tokenizer_path))
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
        write_outputs(output_dir, summary=summary)
        print(json.dumps(summary, ensure_ascii=False, sort_keys=True), flush=True)
        return 0

    config = None
    if args.config_id:
        config = configs.get(str(args.config_id))
        if config is None:
            parser.error(f"unknown config id: {args.config_id}")
    if args.forced_token_mode == "replay" and config is None:
        parser.error("replay requires --config-id")
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
            require_diagnostic=args.forced_token_mode == "replay",
        )
    except (OSError, TemplateValidationError, WorkloadExecutionError) as error:
        print(f"workload_failed:{error}", file=sys.stderr, flush=True)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
