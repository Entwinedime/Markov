#!/usr/bin/env python3
"""HiCache workload identity input-contract audit CLI."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from ..common.io import write_json
from .input_contract_core import DEFAULT_ROLES
from .input_contract_report import InputContractReportOptions, build_report


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """解析 cross-input audit CLI 参数。"""

    parser = argparse.ArgumentParser(
        description="Audit cross-config HiCache workload identity state-model facts."
    )
    parser.add_argument(
        "--source-trace",
        type=Path,
        action="append",
        required=True,
        help="Source Python probe trace path. Can be repeated.",
    )
    parser.add_argument(
        "--target-trace",
        type=Path,
        action="append",
        required=True,
        help="Target Python probe trace path. Can be repeated.",
    )
    parser.add_argument("--source-label", default="source", help="Source label used in the report.")
    parser.add_argument("--target-label", default="target", help="Target label used in the report.")
    parser.add_argument("--role", action="append", default=[], help="fact.role to audit. Can be repeated.")
    parser.add_argument("--output", type=Path, help="Output JSON path. Prints full report when omitted.")
    parser.add_argument("--sample", type=int, default=8, help="Maximum mismatch / issue sample size.")
    return parser.parse_args(argv)


def report_options_from_args(args: argparse.Namespace) -> InputContractReportOptions:
    """把 CLI namespace 转成报告层参数对象。"""

    return InputContractReportOptions(
        source_trace=list(args.source_trace),
        target_trace=list(args.target_trace),
        source_label=str(args.source_label),
        target_label=str(args.target_label),
        roles=tuple(args.role or DEFAULT_ROLES),
        sample=int(args.sample),
    )


def print_report_or_summary(args: argparse.Namespace, report: dict[str, object]) -> None:
    """按 CLI 选项输出完整报告或短摘要。"""

    if args.output:
        write_json(args.output, report)
        summary = {
            "schema": report["schema"],
            "output": str(args.output),
            "source_label": report["source_label"],
            "target_label": report["target_label"],
            "source_event_count": report["source_event_count"],
            "target_event_count": report["target_event_count"],
            "input_contract_ready": report["input_contract_ready"],
            "input_contract_blocking_roles": report["input_contract_blocking_roles"],
            "workload_identity_path_contract_blocking_roles": report["workload_identity_path_contract_blocking_roles"],
            "non_blocking_sequence_mismatch_roles": report["non_blocking_sequence_mismatch_roles"],
            "unknown_workload_identity_roles": report["unknown_workload_identity_roles"],
            "unmapped_request_id_events": report["unmapped_request_id_events"],
        }
        print(json.dumps(summary, ensure_ascii=False, indent=2))
        return
    print(json.dumps(report, ensure_ascii=False, indent=2))


def main(argv: list[str] | None = None) -> int:
    """CLI 入口。"""

    args = parse_args(argv)
    report = build_report(report_options_from_args(args))
    print_report_or_summary(args, report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
