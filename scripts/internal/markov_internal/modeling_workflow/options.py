"""CLI 参数到 workflow options 的转换。"""

from __future__ import annotations

import argparse
from pathlib import Path

from ..common.paths import ROOT_DIR, resolve_repo_path
from .context import WorkflowOptions
from .validations import validation_names


PREDICTION_SCOPE_CHOICES = {"self", "cross"}


class WorkflowOptionsBuilder:
    """把 argparse 结果规整成内部 WorkflowOptions。"""

    def from_args(self, args: argparse.Namespace) -> WorkflowOptions:
        """构造 workflow options。"""

        validations = parse_csv_tuple(args.validations, set(validation_names()), label="validations")
        prediction_scope = parse_csv_tuple(args.prediction_scope, PREDICTION_SCOPE_CHOICES, label="prediction scope")
        return WorkflowOptions(
            profile_run_dirs=tuple(required_repo_path(path) for path in args.profile_run_dir),
            manifests=tuple(required_repo_path(path) for path in args.manifest),
            output_dir=self._output_dir(args),
            validations=validations,
            input_ids=frozenset(parse_selector_values(args.input, args.inputs)),
            config_ids=frozenset(parse_selector_values(args.config, args.configs)),
            source_config_ids=frozenset(parse_selector_values(args.source_config, args.source_configs)),
            target_config_ids=frozenset(parse_selector_values(args.target_config, args.target_configs)),
            prediction_scope=frozenset(prediction_scope),
            dry_run=bool(args.dry_run),
            force=bool(args.force),
            continue_on_error=bool(args.continue_on_error),
            max_predictions=max(0, int(args.max_predictions)),
            page_key_mode=str(args.page_key_mode),
            sample_limit=int(args.sample),
            emit_transition_catalog=bool(args.emit_transition_catalog),
            emit_transition_gates=bool(args.emit_transition_gates),
            show_workload_sequence=bool(args.show_workload_sequence),
        )

    def _output_dir(self, args: argparse.Namespace) -> Path:
        if args.output_dir:
            return required_repo_path(args.output_dir)
        if args.profile_run_dir:
            return required_repo_path(args.profile_run_dir[0]) / "modeling" / "modeling_workflow"
        return ROOT_DIR / "data/modeling_runs/modeling_workflow"


def parse_csv_tuple(raw: str, choices: set[str], *, label: str) -> tuple[str, ...]:
    """解析逗号分隔的 CLI 选择项。"""

    values = tuple(item.strip() for item in raw.split(",") if item.strip())
    if not values:
        raise SystemExit(f"No {label} selected.")
    unknown = sorted(set(values) - choices)
    if unknown:
        raise SystemExit(f"Unknown {label}: {', '.join(unknown)}")
    return values


def parse_selector_values(*groups: list[str]) -> set[str]:
    """解析 repeated 和 comma-separated selector。"""

    result: set[str] = set()
    for group in groups:
        for raw in group:
            for item in str(raw).split(","):
                item = item.strip()
                if item:
                    result.add(item)
    return result


def required_repo_path(path: Path) -> Path:
    """解析必须存在语义上的 repo 路径表达。"""

    resolved = resolve_repo_path(path)
    if resolved is None:
        raise SystemExit("internal error: expected path value")
    return resolved
