#!/usr/bin/env python3
"""HiCache transition exactness 验证入口。

本文件只保留 CLI 参数解析和 mode 分发；单格比较、matrix 编排、路径解析
分别位于 `transition_compare`、`transition_matrix` 和 `transition_paths`。
"""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Any

from ...common.io import write_json
from ...common.paths import ROOT_DIR
from ..matrix.runs.discovery import profile_run_from_manifest
from .artifacts.catalog import (
    build_transition_mismatch_catalog_from_entries,
    write_transition_catalog_outputs,
)
from .validation.compare import compare_prediction_to_observed
from .validation.gate import write_prediction_gate_outputs
from .validation.matrix import compare_transition_matrix
from .validation.oracle import extract_target_oracle
from .artifacts.paths import (
    PathsForPrediction,
    map_repo_path,
    resolve_output,
    resolve_repo_path,
    resolve_required_path,
)
from .validation.self_check import build_model_self_check


DEFAULT_PAGE_KEY_MODE = "strip_scope"
CLI_DESCRIPTION = (
    "Validate HiCache transition exactness from predicted model transitions and target-side Python-probe oracle traces."
)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """解析 CLI 参数。"""

    parser = argparse.ArgumentParser(description=CLI_DESCRIPTION)
    parser.add_argument(
        "--mode",
        required=True,
        choices=(
            "model-self-check",
            "extract-target-oracle",
            "compare-self",
            "compare-cross",
            "compare-matrix",
        ),
    )
    parser.add_argument("--prediction-dir", type=Path, help="Single prediction output directory.")
    parser.add_argument("--predicted-trace", type=Path, help="Explicit predicted_target_cache_state_trace.json path.")
    parser.add_argument("--target-manifest", type=Path, help="Target profile_manifest.json path.")
    parser.add_argument(
        "--oracle-trace",
        type=Path,
        action="append",
        default=[],
        help="Explicit target Python probe trace path. Can be repeated.",
    )
    parser.add_argument("--observed-target-trace", type=Path, help="observed_target_transition_trace.json path.")
    parser.add_argument("--matrix-dir", type=Path, help="hicache_state_workflow output directory.")
    parser.add_argument("--output", type=Path, help="Primary JSON output path. Defaults depend on --mode.")
    parser.add_argument(
        "--catalog-output",
        type=Path,
        help="Output path used by --emit-catalog. Default: transition_mismatch_catalog.json.",
    )
    parser.add_argument(
        "--gate-output",
        type=Path,
        help="Matrix scoreboard output path used by --emit-gates. Default: transition_patch_gate_scoreboard.json.",
    )
    parser.add_argument("--page-key-mode", default=DEFAULT_PAGE_KEY_MODE, choices=("strip_scope", "raw"))
    parser.add_argument("--force", action="store_true", help="Rebuild existing oracle / compare artifacts.")
    parser.add_argument(
        "--emit-catalog",
        action="store_true",
        help="Emit transition mismatch catalog artifacts from compare results.",
    )
    parser.add_argument("--emit-gates", action="store_true", help="Emit operation gate artifacts from compare results.")
    parser.add_argument("--sample", type=int, default=20, help="Maximum mismatch / issue sample size.")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    """CLI 入口。"""

    args = parse_args(argv)
    mode = str(args.mode)
    if mode == "model-self-check":
        prediction_paths = prediction_paths_from_args(args)
        output = resolve_output(args.output, prediction_paths.model_self_check)
        write_json(output, build_model_self_check(prediction_paths.predicted_trace, sample_limit=args.sample))
        return 0
    if mode == "extract-target-oracle":
        trace_paths, target_metadata = target_trace_paths_from_args(args)
        output = resolve_output(args.output, default_target_oracle_output(args, target_metadata))
        write_json(output, extract_target_oracle(trace_paths, target_metadata, sample_limit=args.sample))
        return 0
    if mode in {"compare-self", "compare-cross"}:
        prediction_paths = prediction_paths_from_args(args)
        observed_path = resolve_required_path(args.observed_target_trace, "--observed-target-trace")
        default_name = "transition_exactness_self.json" if mode == "compare-self" else "transition_exactness_cross.json"
        output = resolve_output(args.output, prediction_paths.prediction_dir / default_name)
        comparison = compare_prediction_to_observed(
            prediction_paths,
            observed_path,
            comparison_mode="self" if mode == "compare-self" else "cross",
            page_key_mode=args.page_key_mode,
            sample_limit=args.sample,
            include_classification_evidence=bool(args.emit_catalog),
        )
        write_json(output, comparison)
        emit_single_compare_derivatives(
            args,
            prediction_paths,
            observed_path,
            classification_entry_from_comparison(comparison),
        )
        return 0
    if mode == "compare-matrix":
        matrix_dir = resolve_required_path(args.matrix_dir, "--matrix-dir")
        output = resolve_output(args.output, matrix_dir / "transition_exactness_matrix.json")
        write_json(
            output,
            compare_transition_matrix(
                matrix_dir,
                page_key_mode=args.page_key_mode,
                force=args.force,
                sample_limit=args.sample,
                emit_catalog=args.emit_catalog,
                emit_gates=args.emit_gates,
                catalog_output=args.catalog_output,
                gate_output=args.gate_output,
                matrix_output_path=output,
            ),
        )
        return 0
    raise SystemExit(f"unsupported mode: {mode}")


def classification_entry_from_comparison(comparison: dict[str, Any]) -> dict[str, Any]:
    """从 compare 输出中读取分类 entry。"""

    entry = comparison.get("transition_classification")
    return entry if isinstance(entry, dict) else {}


def emit_single_compare_derivatives(
    args: argparse.Namespace,
    prediction_paths: PathsForPrediction,
    observed_path: Path,
    classification_entry: dict[str, Any],
) -> None:
    """按显式开关写出单格 catalog/gate 派生产物。"""

    if args.emit_catalog:
        catalog_output = resolve_output(
            args.catalog_output,
            prediction_paths.prediction_dir / "transition_mismatch_catalog.json",
        )
        catalog = build_transition_mismatch_catalog_from_entries(
            prediction_paths.prediction_dir,
            [classification_entry],
            source_matrix_path="",
            sample_limit=args.sample,
        )
        write_transition_catalog_outputs(
            prediction_paths.prediction_dir,
            catalog_output,
            catalog,
            sample_limit=args.sample,
        )
    if args.emit_gates:
        write_prediction_gate_outputs(
            prediction_paths.prediction_dir,
            observed_path,
            classification_entry,
            page_key_mode=args.page_key_mode,
            sample_limit=args.sample,
        )


def prediction_paths_from_args(args: argparse.Namespace) -> PathsForPrediction:
    """从 CLI 参数解析 prediction 标准路径。"""

    if args.prediction_dir is None and args.predicted_trace is None:
        raise SystemExit("missing --prediction-dir or --predicted-trace")
    prediction_dir = (
        resolve_repo_path(args.prediction_dir)
        if args.prediction_dir
        else resolve_repo_path(args.predicted_trace).parent
    )
    predicted_trace = (
        resolve_repo_path(args.predicted_trace)
        if args.predicted_trace
        else prediction_dir / "predicted_target_cache_state_trace.json"
    )
    return PathsForPrediction(
        prediction_dir=prediction_dir,
        predicted_trace=predicted_trace,
        validation=prediction_dir / "validation.json",
        model_self_check=prediction_dir / "model_transition_self_check.json",
    )


def default_target_oracle_output(args: argparse.Namespace, target_metadata: dict[str, Any]) -> Path:
    """返回 target oracle 的缺省输出路径。"""

    if args.target_manifest is not None:
        run_dir = Path(str(target_metadata.get("target_run_dir") or "")).expanduser()
        if run_dir:
            return map_repo_path(run_dir) / "modeling" / "observed_target_transition_trace.json"
    return ROOT_DIR / "data/modeling_runs/hicache_transition/observed_target_transition_trace.json"


def target_trace_paths_from_args(args: argparse.Namespace) -> tuple[list[Path], dict[str, Any]]:
    """解析 target profile manifest 或显式 oracle trace。"""

    if args.target_manifest is not None:
        run = profile_run_from_manifest(resolve_repo_path(args.target_manifest))
        return list(run.python_probe_files), {
            "target_manifest_path": str(run.manifest_path),
            "target_run_dir": str(run.run_dir),
            "target_run_id": run.run_id,
            "target_config_id": run.config_id,
            "input_id": run.input_id,
            "input_class": run.input_class,
        }
    trace_paths = [resolve_repo_path(path) for path in args.oracle_trace]
    if not trace_paths:
        raise SystemExit("missing --target-manifest or --oracle-trace")
    return trace_paths, {
        "target_manifest_path": "",
        "target_run_dir": "",
        "target_run_id": "",
        "target_config_id": "",
        "input_id": "",
        "input_class": "",
    }


if __name__ == "__main__":
    raise SystemExit(main())
