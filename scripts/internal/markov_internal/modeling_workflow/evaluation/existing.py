"""Score completed predictions; optional oracle replay is a separate diagnostic."""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import time

from ...common.io import load_json, write_json
from ...common.paths import require_repo_path
from ...modeling.workload import discover_workload_window
from ..artifacts import ModelRunArtifacts, WorkflowArtifactLayout
from ..options import nonnegative_int, positive_int
from ..planning.profile_runs import ProfileRunDiscovery, ProfileRunParser
from ..prediction.hicache import prediction_evidence
from ..types import CacheStatePredictionRef, ModelRunResult, ModelRunSpec, TargetHiCacheConfig
from ..validations.final_dag.shape_oracle import extract_target_shape_oracle, patch_probe_contract_enabled
from ..validations.hicache.phase.score import extract_target_phase_observation
from .scoring import score_cell, score_metrics


def completed_predictions(directory: Path) -> list[tuple[dict, dict]]:
    """Validate the selected execution before any evaluator target inputs are opened."""

    artifacts = WorkflowArtifactLayout(directory)
    summary = load_json(artifacts.workflow_summary_path)
    plan = load_json(artifacts.plan_path)
    cells = plan["model_runs"]
    outcome = summary["prediction"]
    if not cells or outcome["status"] != "READY" or outcome["ready_count"] != len(cells) or summary["model_run_error_count"]:
        raise ValueError(f"prediction execution is incomplete: {directory}")
    sources = {row["run_id"]: row for row in plan["runs"]}
    parsed_sources = {}
    rows = []
    for cell in cells:
        model = ModelRunArtifacts(require_repo_path(cell["output_dir"]))
        if not model.output_dir.resolve().is_relative_to(directory.resolve()):
            raise ValueError(f"prediction output is outside its declared directory: {cell['run_id']}")
        run = load_json(model.run_summary_json)
        evidence = run.get("prediction")
        if evidence is None:
            # Full diagnostics contain the same evidence; project it in memory.
            # This is not a model rerun, an on-disk migration or a version branch.
            source_id = cell["source_run_id"]
            if source_id not in parsed_sources:
                parsed_sources[source_id] = ProfileRunParser().from_manifest(
                    require_repo_path(sources[source_id]["manifest_path"]))
            source = parsed_sources[source_id]
            config = load_json(model.cpp_model_config_json)["hicache"]
            fields = {key: value for key, value in config.items()
                      if key not in {"io_cost", "phase_cost", "kv_bytes_per_page", "dag_patch"}}
            target = TargetHiCacheConfig(cell["target_config"], fields)
            spec = ModelRunSpec(cell["run_id"], source, target, model.output_dir, CacheStatePredictionRef(source, target))
            evidence = prediction_evidence(ModelRunResult(spec, 0, 0., model))
            evidence["is_self"] = cell["prediction"]["is_self"]
        expected = (cell["run_id"], cell["source_config_id"], cell["target_config"], cell["input_id"])
        actual = tuple(evidence[key] for key in ("model_run_id", "source_config_id", "target_config", "workload_id"))
        patch = run["module_results"]["hicache_dag_patch"]
        if actual != expected or evidence["status"] != "READY" or patch["phase_owner_conflict_count"] != 0:
            raise ValueError(f"prediction evidence is not ready or belongs to another cell: {cell['run_id']}")
        if patch["status"] not in {"applied", "no_mutation_required"} or not patch["topology_valid"] or patch["phase_patch_status"] != "ready":
            raise ValueError(f"prediction patch is incomplete: {cell['run_id']}")
        if not evidence["shape"]["effects"] or "source_io_observations" not in run:
            raise ValueError(f"prediction lacks scoring evidence: {cell['run_id']}")
        rows.append((evidence, run))
    return rows


def recorded_costs(directory: Path) -> dict:
    """Expose existing group accounting without inventing a comparable cold-start total."""

    path = directory.parent / "group_summary.json"
    if not path.is_file():
        return {"status": "NOT_RECORDED", "economics_verified": False}
    summary = load_json(path)
    reference = summary.get("prediction_summary")
    if not reference or require_repo_path(reference).resolve() != (directory / "workflow_summary.json").resolve():
        return {"status": "NOT_RECORDED", "economics_verified": False}
    fields = ("prepare_wall_seconds", "planning_wall_seconds", "model_build_wall_seconds", "prediction_wall_seconds",
              "base_acquisition_wall_seconds", "physical_acquisition_wall_seconds",
              "base_capture_usage", "physical_capture_usage", "capture_usage", "physical_support_refresh")
    attempts = summary.get("preparation_attempts", [])
    return {"status": "INCOMPLETE_COST_EVIDENCE", "economics_verified": False,
            "group_summary": str(path), "recorded": {key: summary.get(key) for key in fields},
            "preparation_attempts": attempts,
            "recorded_preparation_wall_seconds": sum(row["wall_seconds"] for row in attempts if row["wall_seconds"] is not None),
            "preparation_history_complete": bool(attempts) and summary.get("preparation_history_complete", False)
                and all(row["wall_seconds"] is not None for row in attempts),
            "interpretation": "prepare contains this invocation's stages; cumulative acquisition ledgers may overlap it",
            "missing": ["complete first-use costs of previously available assets", "same-clock target measurement baseline"]}


def evaluate(prediction_dirs: list[Path], profile_dirs: list[Path], manifests: list[Path], output: Path,
             *, jobs: int = 1, threads: int = 1, file_threads: int = 1,
             oracle_cost_replay: bool = False, oracle_max_runs: int = 0) -> dict:
    if any(output.resolve().is_relative_to(path.resolve()) or path.resolve().is_relative_to(output.resolve())
           for path in prediction_dirs):
        raise ValueError("evaluation output must be separate from prediction inputs")
    write_json(output / "gate_summary.json", {"status": "SCORING", "new_predictions": 0, "new_acquisitions": 0})
    try:
        return _evaluate(prediction_dirs, profile_dirs, manifests, output, jobs, threads, file_threads, oracle_cost_replay, oracle_max_runs)
    except Exception as error:
        write_json(output / "gate_summary.json", {"status": "FAILED", "error": str(error),
                                                  "new_predictions": 0, "new_acquisitions": 0})
        raise


def _evaluate(prediction_dirs, profile_dirs, manifests, output, jobs, threads, file_threads, oracle_cost_replay, oracle_max_runs):
    started = time.monotonic()
    # Complete every selected group before discovering even the first target.
    predictions = [(directory, row, run) for directory in prediction_dirs
                   for row, run in completed_predictions(directory)]
    keys = [(row["source_config_id"], row["target_config"], row["workload_id"]) for _, row, _ in predictions]
    if len(keys) != len(set(keys)):
        raise ValueError("evaluation received duplicate prediction cells")
    expected = {(row["target_config"], row["workload_id"]) for _, row, _ in predictions}
    targets = {}
    for target in ProfileRunDiscovery(tuple(profile_dirs), tuple(manifests)).discover():
        key = (target.config_id, target.input_id)
        if key not in expected:
            continue
        if key in targets:
            raise ValueError(f"ambiguous target profiles for {key}; select explicit manifests")
        targets[key] = target
    if missing := expected - targets.keys():
        raise ValueError(f"evaluation has no target profiles for: {sorted(missing)}")
    for _, row, _ in predictions:
        target = targets[row["target_config"], row["workload_id"]]
        actual = {"enabled": True, **(target.hicache_config or {})}
        if any(actual.get(key) != value for key, value in row["target_hicache"].items()):
            raise ValueError(f"target profile configuration differs from predicted configuration: {target.label}")

    layout = WorkflowArtifactLayout(output)
    new_extractions = sum(not (layout.target_phase_score_dir(target.run_id) / "run_summary.phase_carrier.json").is_file()
                          for target in targets.values())
    def observe(target):
        observed = extract_target_phase_observation(target, layout.target_phase_score_dir(target.run_id),
                                                   threads=threads, file_threads=file_threads)
        window = discover_workload_window({}, target.manifest_path)
        oracle = extract_target_shape_oracle(target.python_probe_files,
            patch_probe_contract_ready=patch_probe_contract_enabled(load_json(target.manifest_path)),
            window_start_us=window.start_ns // 1000 if window else None,
            window_end_us=window.end_ns // 1000 if window else None)
        print(f"observed target: {target.label}", flush=True)
        return (target.config_id, target.input_id), (observed, oracle)

    with ThreadPoolExecutor(max_workers=jobs) as executor:
        observations = dict(executor.map(observe, targets.values()))
    rows = []
    for _, prediction, run in predictions:
        key = (prediction["target_config"], prediction["workload_id"])
        row = score_cell(
            prediction,
            run,
            *observations[key],
            targets[key].run_id,
            include_oracle_costs=oracle_cost_replay,
        )
        write_json(output / "cells" / f"{row['model_run_id']}.json", row)
        rows.append(row)
    cross, self_rows = [row for row in rows if not row["is_self"]], [row for row in rows if row["is_self"]]
    by_source = {base: score_metrics([row for row in cross if row["source_config_id"] == base])
                 for base in sorted({row["source_config_id"] for row in cross})}
    result = {**score_metrics(cross), "by_source": by_source,
              "self_check": score_metrics(self_rows) if self_rows else None,
              "prediction_directories": [str(path) for path in prediction_dirs],
              "predictions_completed": len(rows), "new_predictions": 0, "new_acquisitions": 0,
              "target_profile_count": len(targets), "evaluation_wall_seconds": time.monotonic() - started,
              "new_target_extractions": new_extractions, "target_observations_reused": len(targets) - new_extractions,
              "costs": {str(path): recorded_costs(path) for path in prediction_dirs},
              "target_opened_after_all_selected_predictions": True, "parameters_or_capture_plan_changed": False,
              "formal_and_owner_scope": "current canonical Direct+phase, residual gap/unowned cost excluded"}
    if any(group["status"] != "PASS" for group in by_source.values()):
        result["status"] = "MODEL_LIMITATION"
    if oracle_cost_replay:
        from ..validations.hicache.oracle_cost_replay.runner import run_suite

        roots = {row["model_run_id"]: directory for directory, row, _ in predictions}
        source_runs = {row["model_run_id"]: run for _, row, run in predictions}
        target_runs = {target.run_id: observations[key][0] for key, target in targets.items()}
        replay_started = time.monotonic()
        result["oracle_cost_replay"] = {
            base: run_suite(roots, rows, source_runs, target_runs, output, source_config_id=base,
                            jobs=jobs, max_runs=oracle_max_runs or None)
            for base in by_source
        }
        result["oracle_replay_wall_seconds"] = time.monotonic() - replay_started
        result["new_oracle_cpp_replays"] = sum(item["cpp_replay_count"] for item in result["oracle_cost_replay"].values())
    write_json(output / "gate_summary.json", result)
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prediction-dir", type=Path, action="append", required=True,
                        help="Completed prediction output; repeat for multiple base groups.")
    parser.add_argument("--profile-run-dir", type=Path, action="append", default=[], help="Score-only target profile suite.")
    parser.add_argument("--target-profile-manifest", type=Path, action="append", default=[], help="Explicit score-only target.")
    parser.add_argument("--output-dir", type=Path, required=True, help="Separate evaluation output, never the prediction directory.")
    parser.add_argument("--model-run-jobs", type=positive_int, default=1, help="Concurrent target extractions, not predictions.")
    parser.add_argument("--trace-threads", type=positive_int, default=1)
    parser.add_argument("--trace-file-threads", type=positive_int, default=1)
    parser.add_argument("--oracle-cost-replay", action="store_true",
                        help="Replay costs from this evaluation's target observations; requires full prediction diagnostics.")
    parser.add_argument("--oracle-max-runs", type=nonnegative_int, default=0,
                        help="Diagnostic cells per base; 0 is all. Does not limit ordinary scoring.")
    args = parser.parse_args(argv)
    if args.oracle_max_runs and not args.oracle_cost_replay:
        parser.error("--oracle-max-runs requires --oracle-cost-replay")
    result = evaluate([require_repo_path(path) for path in args.prediction_dir],
                      [require_repo_path(path) for path in args.profile_run_dir],
                      [require_repo_path(path) for path in args.target_profile_manifest], require_repo_path(args.output_dir),
                      jobs=args.model_run_jobs, threads=args.trace_threads, file_threads=args.trace_file_threads,
                      oracle_cost_replay=args.oracle_cost_replay, oracle_max_runs=args.oracle_max_runs)
    print(f"evaluation={result['status']} cross={result['cell_count']} new_predictions=0", flush=True)
    replay_ready = all(item["ready_count"] == item["selected_cell_count"] for item in result.get("oracle_cost_replay", {}).values())
    return 0 if result["status"] == "PASS" and replay_ready else 2


if __name__ == "__main__":
    raise SystemExit(main())
