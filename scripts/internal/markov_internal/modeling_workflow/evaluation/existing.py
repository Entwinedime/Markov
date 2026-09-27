"""Score completed predictions; optional oracle replay is a separate diagnostic."""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import time

from ...common.io import load_json, write_json
from ...common.commands import nonnegative_int, positive_int
from ...common.paths import require_repo_path
from ...modeling.workload import discover_workload_window
from ..planning.profile_runs import discover_profile_runs, parse_profile_run
from ..prediction.hicache import execution_blockers
from ..validations.final_dag.shape_oracle import extract_target_shape_oracle, patch_probe_contract_enabled
from ..validations.hicache.oracle_cost_replay.evidence import extract_target_phase_observation, prepare_replay_evidence
from .http_metrics import score_cell, score_metrics


def completed_predictions(directory: Path) -> list[tuple[dict, dict]]:
    """Validate the selected execution before any evaluator target inputs are opened."""

    summary = load_json(directory / "workflow_summary.json")
    outcome = summary["prediction"]
    if outcome["status"] not in {"READY", "EXECUTED"} or summary["model_run_error_count"]:
        raise ValueError(f"prediction execution is incomplete: {directory}")

    cells = outcome.get("cells", [])
    # Retained historical results split identity between a task index and each
    # run. New predictions keep it only in the workflow summary.
    retained = not cells or "source_manifest" not in cells[0]
    if retained:
        plan = load_json(directory / "artifacts" / "model_run_plan.json")
        cells = plan["model_runs"]
        sources = {row["run_id"]: row for row in plan["runs"]}

    if not cells or outcome.get("completed_count", outcome.get("ready_count")) != len(cells):
        raise ValueError(f"prediction execution is incomplete: {directory}")
    rows = []
    for cell in cells:
        run_id = cell["run_id"] if retained else cell["model_run_id"]
        output = require_repo_path(cell["output_dir"]) if retained else directory / "model_runs" / run_id
        if not output.resolve().is_relative_to(directory.resolve()):
            raise ValueError(f"prediction output is outside its declared directory: {run_id}")
        run = load_json(output / "run_summary.json")
        if retained:
            evidence = run.get("prediction")
            if not isinstance(evidence, dict):
                raise ValueError(
                    f"prediction lacks retained scoring evidence: {run_id}; run the formal prediction entrypoint"
                )
            expected = (run_id, cell["source_config_id"], cell["target_config"], cell["input_id"])
            actual = tuple(
                evidence[key] for key in ("model_run_id", "source_config_id", "target_config", "workload_id")
            )
            if actual != expected:
                raise ValueError(f"prediction evidence belongs to another cell: {run_id}")
            evidence = {**evidence, "source_manifest": sources[cell["source_run_id"]]["manifest_path"]}
        else:
            evidence = dict(cell)

        if "hicache_execution" in run["module_results"]:
            execution = run["module_results"]["hicache_execution"]
            if evidence["status"] != "EXECUTED" or execution_blockers(execution):
                raise ValueError(f"prediction execution is incomplete or belongs to another cell: {run_id}")

            # The C++ result is authoritative. Do not maintain or validate a
            # second serialized copy inside prediction metadata.
            evidence["execution"] = execution
        else:
            patch = run["module_results"]["hicache_dag_patch"]
            if evidence["status"] != "READY" or patch["phase_owner_conflict_count"] != 0:
                raise ValueError(f"prediction evidence is not ready: {run_id}")
            if (
                patch["status"] not in {"applied", "no_mutation_required"}
                or not patch["topology_valid"]
                or patch["phase_patch_status"] != "ready"
            ):
                raise ValueError(f"prediction patch is incomplete: {run_id}")
            if not evidence["shape"]["effects"] or "source_io_observations" not in run:
                raise ValueError(f"prediction lacks scoring evidence: {run_id}")

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
    fields = (
        "prepare_wall_seconds",
        "model_preparation_wall_seconds",
        "prediction_wall_seconds",
        "base_capture_usage",
        "physical_capture_usage",
        "calibration_usage",
    )
    attempts = summary.get("preparation_attempts", [])
    return {
        "status": "INCOMPLETE_COST_EVIDENCE",
        "economics_verified": False,
        "group_summary": str(path),
        "recorded": {key: summary.get(key) for key in fields},
        "preparation_attempts": attempts,
        "recorded_preparation_wall_seconds": sum(
            row["wall_seconds"] for row in attempts if row["wall_seconds"] is not None
        ),
        "interpretation": "prepare contains this invocation's stages; cumulative acquisition ledgers may overlap it",
        "missing": [
            "complete first-use costs of previously available assets",
            "same-clock target measurement baseline",
        ],
    }


def evaluate(
    prediction_dirs: list[Path],
    profile_dirs: list[Path],
    manifests: list[Path],
    output: Path,
    *,
    jobs: int = 1,
    threads: int = 1,
    file_threads: int = 1,
    oracle_cost_replay: bool = False,
    oracle_max_runs: int = 0,
    normal_http: bool = False,
) -> dict:
    if any(
        output.resolve().is_relative_to(path.resolve()) or path.resolve().is_relative_to(output.resolve())
        for path in prediction_dirs
    ):
        raise ValueError("evaluation output must be separate from prediction inputs")
    write_json(output / "gate_summary.json", {"status": "SCORING", "new_predictions": 0, "new_acquisitions": 0})
    try:
        return _evaluate(
            prediction_dirs,
            profile_dirs,
            manifests,
            output,
            jobs,
            threads,
            file_threads,
            oracle_cost_replay,
            oracle_max_runs,
            normal_http,
        )
    except Exception as error:
        write_json(
            output / "gate_summary.json",
            {"status": "FAILED", "error": str(error), "new_predictions": 0, "new_acquisitions": 0},
        )
        raise


def _evaluate(
    prediction_dirs: list[Path],
    profile_dirs: list[Path],
    manifests: list[Path],
    output: Path,
    jobs: int,
    threads: int,
    file_threads: int,
    oracle_cost_replay: bool,
    oracle_max_runs: int,
    normal_http: bool,
) -> dict:
    started = time.monotonic()
    # Complete every selected group before discovering even the first target.
    predictions = [
        (directory, row, run) for directory in prediction_dirs for row, run in completed_predictions(directory)
    ]
    if oracle_cost_replay and any("execution" in row for _, row, _ in predictions):
        raise ValueError("execution cost replay requires target operation mapping, which is not yet available")
    if normal_http and any("execution" not in row for _, row, _ in predictions):
        raise ValueError("normal HTTP scoring requires execution predictions; static diagnostics require target traces")
    keys = [(row["source_config_id"], row["target_config"], row["workload_id"]) for _, row, _ in predictions]
    if len(keys) != len(set(keys)):
        raise ValueError("evaluation received duplicate prediction cells")
    expected = {(row["target_config"], row["workload_id"]) for _, row, _ in predictions}
    if normal_http:
        expected.update((row["source_config_id"], row["workload_id"]) for _, row, _ in predictions)
    targets = {}
    samples = {}
    request_orders = {}
    for target in discover_profile_runs(tuple(manifests), profile_run_dirs=tuple(profile_dirs)):
        key = (target.config_id, target.input_id)
        if key not in expected:
            continue
        if key in targets and not normal_http:
            raise ValueError(f"ambiguous target profiles for {key}; select explicit manifests")
        if normal_http:
            config = load_json(target.config_path)
            profiling = config["profiling"]
            if (
                profiling["enabled"] is not False
                or profiling["channels"]
                or any("TIMING" in k for k in config.get("env", {}))
            ):
                raise ValueError(
                    f"normal HTTP measurement contains profiling or timing instrumentation: {target.label}"
                )
            window = target.workload_window
            if window is None:
                raise ValueError(f"normal HTTP measurement has no formal window: {target.label}")
            report = load_json(window.report_path)
            requests = [row for row in report["requests"] if row.get("measure")]
            if (
                report["status"] != "completed"
                or report["formal_window"]["status"] != "ok"
                or not requests
                or any(
                    row["http_status"] != 200
                    or row.get("actual_output_matches_forced") is False
                    or row.get("actual_prompt_matches_plan") is False
                    for row in requests
                )
            ):
                raise ValueError(f"normal HTTP measurement has failed requests or token replay: {target.label}")
            order = [row["logical_request_id"] for row in requests]
            if key in request_orders and order != request_orders[key]:
                raise ValueError(f"normal HTTP repetitions have different request order: {target.label}")
            request_orders[key] = order
        samples.setdefault(key, []).append(target)
        targets.setdefault(key, target)
    if missing := expected - targets.keys():
        raise ValueError(f"evaluation has no target profiles for: {sorted(missing)}")
    configurations = [((row["target_config"], row["workload_id"]), row["target_hicache"]) for _, row, _ in predictions]
    if normal_http:
        bound = {key for key, _ in configurations}
        for _, row, _ in predictions:
            key = (row["source_config_id"], row["workload_id"])
            if key not in bound:
                configurations.append(
                    (key, parse_profile_run(require_repo_path(row["source_manifest"])).hicache_config)
                )
                bound.add(key)
    for key, configuration in configurations:
        for target in samples[key]:
            actual = {"enabled": True, **(target.hicache_config or {})}
            if any(actual.get(field) != value for field, value in configuration.items()):
                raise ValueError(f"target profile configuration differs from predicted configuration: {target.label}")

    phase_scores = output / "artifacts" / "target_phase_scores"

    def observe(target):
        window = target.workload_window
        observed = extract_target_phase_observation(
            target, phase_scores / target.run_id, threads=threads, file_threads=file_threads
        )
        oracle = extract_target_shape_oracle(
            target.python_probe_files,
            patch_probe_contract_ready=patch_probe_contract_enabled(load_json(target.manifest_path)),
            window_start_us=window.start_ns // 1000 if window else None,
            window_end_us=window.end_ns // 1000 if window else None,
        )
        print(f"observed target: {target.label}", flush=True)
        return (target.config_id, target.input_id), (observed, oracle)

    observations = {}
    new_extractions = 0
    if oracle_cost_replay:
        new_extractions = sum(
            not (phase_scores / target.run_id / "run_summary.phase_carrier.json").is_file()
            for target in targets.values()
        )
        with ThreadPoolExecutor(max_workers=jobs) as executor:
            observations = dict(executor.map(observe, targets.values()))

    rows = []
    source_windows = {}
    for _, prediction, run in predictions:
        key = (prediction["target_config"], prediction["workload_id"])
        source_manifest = prediction["source_manifest"]
        if normal_http:
            source_key = (prediction["source_config_id"], prediction["workload_id"])
            order = [row["request_id"] for row in run["http_client"]["requests"]]
            if order != request_orders[key] or order != request_orders[source_key]:
                raise ValueError(
                    f"normal HTTP measurement request order differs from prediction: {prediction['model_run_id']}"
                )
            base_windows = tuple(sample.workload_window for sample in samples[source_key])
        elif source_manifest not in source_windows:
            source_windows[source_manifest] = discover_workload_window({}, require_repo_path(source_manifest))
        if not normal_http:
            base_windows = (source_windows[source_manifest],)
        row = score_cell(
            prediction,
            run,
            targets[key].run_id,
            source_windows=base_windows,
            target_windows=tuple(sample.workload_window for sample in samples[key])
            if normal_http
            else (targets[key].workload_window,),
        )
        if oracle_cost_replay:
            row.update(prepare_replay_evidence(prediction, run, *observations[key]))
        row["full_e2e"]["measurement_mode"] = "normal_http" if normal_http else "profiled_http"
        write_json(output / "cells" / f"{row['model_run_id']}.json", row)
        rows.append(row)
    cross, self_rows = [row for row in rows if not row["is_self"]], [row for row in rows if row["is_self"]]
    by_source = {
        base: score_metrics([row for row in cross if row["source_config_id"] == base])
        for base in sorted({row["source_config_id"] for row in cross})
    }
    result = {
        **score_metrics(cross),
        "by_source": by_source,
        "self_check": score_metrics(self_rows) if self_rows else None,
        "prediction_directories": [str(path) for path in prediction_dirs],
        "predictions_completed": len(rows),
        "new_predictions": 0,
        "new_acquisitions": 0,
        "target_profile_count": len(targets),
        "measurement_sample_count": sum(len(group) for group in samples.values()),
        "measurement_mode": "normal_http" if normal_http else "profiled_http",
        "evaluation_wall_seconds": time.monotonic() - started,
        "new_target_extractions": new_extractions,
        "target_observations_reused": len(observations) - new_extractions,
        "http_only_target_count": len(targets) - len(observations),
        "costs": {str(path): recorded_costs(path) for path in prediction_dirs},
        "target_opened_after_all_selected_predictions": True,
        "parameters_or_capture_plan_changed": False,
        "formal_and_owner_scope": "full HTTP formal window without exclusions; optional static oracle replay is diagnostic only",
    }
    if any(group["status"] != "PASS" for group in by_source.values()):
        result["status"] = "MODEL_LIMITATION"
    if oracle_cost_replay:
        from ..validations.hicache.oracle_cost_replay.runner import run_suite

        roots = {row["model_run_id"]: directory for directory, row, _ in predictions}
        source_runs = {row["model_run_id"]: run for _, row, run in predictions}
        target_runs = {target.run_id: observations[key][0] for key, target in targets.items()}
        replay_started = time.monotonic()
        result["oracle_cost_replay"] = {
            base: run_suite(
                roots,
                rows,
                source_runs,
                target_runs,
                output,
                source_config_id=base,
                jobs=jobs,
                max_runs=oracle_max_runs or None,
            )
            for base in by_source
        }
        result["oracle_replay_wall_seconds"] = time.monotonic() - replay_started
        result["new_oracle_cpp_replays"] = sum(
            item["cpp_replay_count"] for item in result["oracle_cost_replay"].values()
        )
    write_json(output / "gate_summary.json", result)
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--prediction-dir",
        type=Path,
        action="append",
        required=True,
        help="Completed prediction output; repeat for multiple base groups.",
    )
    parser.add_argument(
        "--profile-run-dir", type=Path, action="append", default=[], help="Score-only target profile suite."
    )
    parser.add_argument(
        "--target-profile-manifest", type=Path, action="append", default=[], help="Explicit score-only target."
    )
    parser.add_argument(
        "--normal-http",
        action="store_true",
        help="Score execution predictions against uninstrumented repetitions; include normal base measurements too.",
    )
    parser.add_argument(
        "--output-dir", type=Path, required=True, help="Separate evaluation output, never the prediction directory."
    )
    parser.add_argument(
        "--model-run-jobs", type=positive_int, default=1, help="Concurrent target extractions, not predictions."
    )
    parser.add_argument("--trace-threads", type=positive_int, default=1)
    parser.add_argument("--trace-file-threads", type=positive_int, default=1)
    parser.add_argument(
        "--oracle-cost-replay",
        action="store_true",
        help="Replay costs from this evaluation's target observations; requires full prediction diagnostics.",
    )
    parser.add_argument(
        "--oracle-max-runs",
        type=nonnegative_int,
        default=0,
        help="Diagnostic cells per base; 0 is all. Does not limit ordinary scoring.",
    )
    args = parser.parse_args(argv)
    if args.oracle_max_runs and not args.oracle_cost_replay:
        parser.error("--oracle-max-runs requires --oracle-cost-replay")
    result = evaluate(
        [require_repo_path(path) for path in args.prediction_dir],
        [require_repo_path(path) for path in args.profile_run_dir],
        [require_repo_path(path) for path in args.target_profile_manifest],
        require_repo_path(args.output_dir),
        jobs=args.model_run_jobs,
        threads=args.trace_threads,
        file_threads=args.trace_file_threads,
        oracle_cost_replay=args.oracle_cost_replay,
        oracle_max_runs=args.oracle_max_runs,
        normal_http=args.normal_http,
    )
    print(f"evaluation={result['status']} cross={result['cell_count']} new_predictions=0", flush=True)
    replay_ready = all(
        item["ready_count"] == item["selected_cell_count"] for item in result.get("oracle_cost_replay", {}).values()
    )
    return 0 if result["status"] == "PASS" and replay_ready else 2


if __name__ == "__main__":
    raise SystemExit(main())
