"""Score completed predictions; optional oracle replay is a separate diagnostic."""

from __future__ import annotations

import argparse
from concurrent.futures import ProcessPoolExecutor, ThreadPoolExecutor
from contextlib import nullcontext
from pathlib import Path
from tempfile import TemporaryDirectory
import time
from typing import Any

from markov_internal.applications.comparison import cost_query_domain
from markov_internal.common.io import load_json, write_json
from markov_internal.common.commands import nonnegative_int, positive_int
from markov_internal.common.paths import require_repo_path
from markov_internal.modeling.workload import discover_workload_window
from markov_internal.modeling.source_dag.profiles import ProfileRunRef, discover_profile_runs, parse_profile_run
from markov_internal.preparation.capture import GROUP_CAPTURE_LEDGERS
from markov_internal.modeling.execution.results import execution_blockers
from markov_internal.modeling.context import DiagnosticLevel
from markov_internal.modeling.sglang_config import canonical_hicache
from markov_internal.applications.accuracy.shape.shape_oracle import (
    extract_target_shape_oracle,
    patch_probe_contract_enabled,
)
from markov_internal.applications.accuracy.oracle_cost_replay.evidence import (
    extract_target_phase_observation,
    prepare_replay_evidence,
)
from markov_internal.applications.accuracy.http_metrics import (
    configuration_selection,
    score_cell,
    score_metrics,
)
from markov_internal.applications.accuracy.structure import compare_structure, extract_target_structure


def _compare_structure_files(predicted: Path, observed: Path, details: bool) -> dict:
    """Load one pair inside its worker; do not pickle full graphs between processes."""

    return compare_structure(load_json(predicted), load_json(observed), details=details)


def completed_predictions(
    directory: Path, selected_targets: set[str] | None = None
) -> tuple[list[tuple[dict, dict]], list[dict]]:
    """Partition current attempts before reading targets or failed-run artifacts.

    Historical static results retain their all-ready input contract. Current
    execution summaries identify failures directly, including unstarted cells.
    """

    summary = load_json(directory / "workflow_summary.json")
    outcome = summary["prediction"]
    cells = outcome.get("cells", [])
    # Retained historical results split identity between a task index and each
    # run. New predictions keep it only in the workflow summary.
    retained = not cells or "source_manifest" not in cells[0]
    if retained:
        if outcome["status"] not in {"READY", "EXECUTED"} or summary["model_run_error_count"]:
            raise ValueError(f"prediction execution is incomplete: {directory}")
        plan = load_json(directory / "artifacts" / "model_run_plan.json")
        cells = plan["model_runs"]
        sources = {row["run_id"]: row for row in plan["runs"]}

    if not cells or (retained and outcome.get("completed_count", outcome.get("ready_count")) != len(cells)):
        raise ValueError(f"prediction execution is incomplete: {directory}")
    if selected_targets is not None:
        cells = [cell for cell in cells if cell["target_config"] in selected_targets]
        if not cells:
            return [], []

    rows = []
    unfinished = []
    for cell in cells:
        if not retained and cell["status"] != "EXECUTED":
            unfinished.append({"prediction_directory": str(directory), **cell})
            continue

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
    return rows, unfinished


def select_prediction_attempts(
    directories: list[Path], retries: list[Path], selected_targets: set[str] | None = None
) -> tuple[list[tuple[Path, dict, dict]], list[dict], list[dict]]:
    """Apply explicit retries only to incomplete cells, before opening targets.

    Match source manifest and run identity, retaining the original target
    configuration. A failed retry remains incomplete in the same denominator;
    an already completed prediction cannot be replaced to select a better fit.
    This selects recorded executions, not their costs or accuracy.
    """

    predictions = []
    unfinished = []
    for directory in directories:
        completed, failed = completed_predictions(directory, selected_targets)
        predictions.extend((directory, row, run) for row, run in completed)
        unfinished.extend(failed)

    pending = {(require_repo_path(row["source_manifest"]).resolve(), row["model_run_id"]): row for row in unfinished}
    if len(pending) != len(unfinished):
        raise ValueError("select one original attempt per incomplete prediction cell")

    superseded = []
    for directory in retries:
        completed, failed = completed_predictions(directory, selected_targets)
        attempts = completed + [(row, None) for row in failed]

        for row, run in attempts:
            key = require_repo_path(row["source_manifest"]).resolve(), row["model_run_id"]
            previous = pending.pop(key)
            if row["target_hicache"] != previous["target_hicache"]:
                raise ValueError(f"retry changes the declared target configuration: {row['model_run_id']}")

            superseded.append(
                {
                    "model_run_id": row["model_run_id"],
                    "previous_directory": previous["prediction_directory"],
                    "previous_status": previous["status"],
                    "previous_blockers": previous["blockers"],
                    "replacement_directory": str(directory),
                    "replacement_status": row["status"],
                }
            )
            if run is None:
                pending[key] = row
            else:
                predictions.append((directory, row, run))

    return predictions, list(pending.values()), superseded


def recorded_costs(directory: Path) -> dict[str, Any]:
    """Read invocation times and existing ledgers; never infer missing costs as zero.

    Invocation wall time overlaps cumulative acquisition usage. Earlier CPU-only
    processing and reused assets are not reconstructed from absent history.
    """

    path = directory.parent / "group_summary.json"
    if not path.is_file():
        return {"status": "NOT_RECORDED", "economics_verified": False}
    summary = load_json(path)
    reference = summary.get("prediction_summary")
    if not reference or require_repo_path(reference).resolve() != (directory / "workflow_summary.json").resolve():
        return {"status": "NOT_RECORDED", "economics_verified": False}

    acquisitions = {}
    for kind, name in GROUP_CAPTURE_LEDGERS.items():
        ledger_path = path.parent / name
        if ledger_path.is_file():
            ledger = load_json(ledger_path)
            acquisitions[kind] = {"ledger": str(ledger_path), "usage": ledger.get("usage")}

    return {
        "status": "INCOMPLETE_COST_EVIDENCE",
        "economics_verified": False,
        "group_summary": str(path),
        "recorded": {
            "current_invocation": {
                "prepare_wall_seconds": summary.get("prepare_wall_seconds"),
                "last_prediction_attempt_wall_seconds": summary.get("prediction_wall_seconds"),
            },
            "cumulative_acquisitions": acquisitions,
        },
        "interpretation": (
            "Invocation wall time and cumulative ledger usage overlap; do not add them. "
            "Only existing ledgers are listed. An absent ledger or null value is unrecorded, not zero cost."
        ),
        "missing": [
            "earlier CPU-only and offline processing invocations not recorded by acquisition ledgers",
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
    structure_manifests: list[Path] | None = None,
    diagnostics: DiagnosticLevel = DiagnosticLevel.OFF,
    retry_dirs: list[Path] | None = None,
    selected_targets: set[str] | None = None,
) -> dict:
    if any(
        output.resolve().is_relative_to(path.resolve()) or path.resolve().is_relative_to(output.resolve())
        for path in [*prediction_dirs, *(retry_dirs or [])]
    ):
        raise ValueError("evaluation output must be separate from prediction inputs")
    write_json(output / "gate_summary.json", {"status": "SCORING", "new_predictions": 0, "new_acquisitions": 0})
    try:
        # Structure extraction is working storage, not another result copy.
        # Retain it only on an explicit detailed-diagnostics request; a failure
        # must also release this attempt's temporary files, never input assets.
        storage = (
            nullcontext(output / "artifacts" / "target_structure")
            if not structure_manifests or diagnostics.keep_debug_artifacts
            else TemporaryDirectory(prefix="target_structure_", dir=output)
        )
        with storage as structure_root:
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
                structure_manifests or [],
                diagnostics,
                retry_dirs or [],
                Path(structure_root),
                selected_targets,
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
    structure_manifests: list[Path],
    diagnostics: DiagnosticLevel,
    retry_dirs: list[Path],
    structure_root: Path,
    selected_targets: set[str] | None,
) -> dict:
    started = time.monotonic()
    # Complete every selected group before discovering even the first target.
    predictions, unfinished, superseded = select_prediction_attempts(prediction_dirs, retry_dirs, selected_targets)
    prediction_dirs = [*prediction_dirs, *retry_dirs]
    if selected_targets is not None:
        found = {row["target_config"] for _, row, _ in predictions} | {row["target_config"] for row in unfinished}
        if found != selected_targets:
            raise ValueError(
                f"selected target configurations are absent from predictions: {sorted(selected_targets - found)}"
            )

    if unfinished:
        result = {
            "status": "NOT_READY",
            "reason": "Selected predictions are incomplete; no target inputs were opened and no accuracy metrics were calculated.",
            "prediction_directories": [str(path) for path in prediction_dirs],
            "predictions_selected": len(predictions) + len(unfinished),
            "predictions_completed": len(predictions),
            "predictions_incomplete": len(unfinished),
            "incomplete_cells": unfinished,
            "superseded_prediction_attempts": superseded,
            "accuracy_scope": "not_scored",
            "target_inputs_opened": False,
            "new_predictions": 0,
            "new_acquisitions": 0,
        }
        write_json(output / "gate_summary.json", result)
        return result
    training_manifests = set()
    unrecorded_training = []
    experiment_report_directories = set()
    for directory in prediction_dirs:
        provenance = load_json(directory / "workflow_summary.json")
        if provenance.get("experiment_reports", False):
            experiment_report_directories.add(directory)
        if "cost_training_manifests" not in provenance:
            unrecorded_training.append(str(directory))
            continue
        training_manifests.update(require_repo_path(path).resolve() for path in provenance["cost_training_manifests"])
    if training_manifests & {require_repo_path(row["source_manifest"]).resolve() for _, row, _ in predictions}:
        raise ValueError("independent training includes a base capture from this evaluation round")
    if structure_manifests and set(prediction_dirs) - experiment_report_directories:
        raise ValueError(
            "structure comparison requires predictions run with --experiment-reports; existing files alone are not current evidence"
        )
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
        if target.manifest_path.resolve() in training_manifests:
            raise ValueError(f"performance evaluation capture was used for cost training: {target.manifest_path}")
        if key in targets and not normal_http:
            raise ValueError(f"ambiguous target profiles for {key}; select explicit manifests")
        if normal_http:
            config = load_json(target.config_path)
            profiling = config["profiling"]
            if (
                profiling["enabled"] is not False
                or profiling["channels"]
                # Explicitly disabled switches do not record timings. Output
                # paths and unknown timing settings still disqualify a normal run.
                or any(
                    "TIMING" in key and not (key.endswith(("_TIMING", "_TIMING_EMISSION")) and str(value) == "0")
                    for key, value in config.get("env", {}).items()
                )
            ):
                raise ValueError(
                    f"normal HTTP measurement contains profiling or active timing instrumentation: {target.label}"
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
        expected_fields = dict(canonical_hicache(configuration)) if normal_http else configuration
        for target in samples[key]:
            actual = {"enabled": True, **(target.hicache_config or {})}
            if normal_http:
                actual = dict(canonical_hicache(actual))
            if any(actual.get(field) != value for field, value in expected_fields.items()):
                raise ValueError(f"target profile configuration differs from predicted configuration: {target.label}")

    structures = {}
    if structure_manifests:
        # This is a separate traced input set. Normal HTTP repetitions remain
        # the performance truth, never a source of graph structure or costs.
        required = {(row["target_config"], row["workload_id"]) for _, row, _ in predictions}
        selected = {}
        for target in discover_profile_runs(tuple(structure_manifests)):
            key = (target.config_id, target.input_id)
            if key not in required:
                continue
            if target.manifest_path.resolve() in training_manifests:
                raise ValueError(f"structure evaluation capture was used for cost training: {target.manifest_path}")
            if key in selected:
                raise ValueError(f"ambiguous target structure profiles for {key}; select one explicit capture")
            actual = {"enabled": True, **(target.hicache_config or {})}
            for selected_key, configuration in configurations:
                if key == selected_key and any(actual.get(field) != value for field, value in configuration.items()):
                    raise ValueError(f"target structure configuration differs from prediction: {target.label}")
            selected[key] = target
        if not selected:
            raise ValueError("selected structure profiles do not match any prediction target")

        def observe_structure(item: tuple[tuple[str, str], ProfileRunRef]) -> tuple[tuple[str, str], Path]:
            key, target = item
            target_output = structure_root / target.run_id
            extract_target_structure(
                target,
                target_output,
                threads=threads,
                file_threads=file_threads,
            )
            return key, target_output / "structure_observation.json"

        with ThreadPoolExecutor(max_workers=jobs) as executor:
            structures = dict(executor.map(observe_structure, selected.items()))

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

    structure_scores = {}
    if structures:
        # Reachability is CPU-bound Python work. A bounded process pool uses
        # the existing job budget without retaining every target graph in the
        # parent or transferring those graphs through multiprocessing pipes.
        with ProcessPoolExecutor(max_workers=jobs) as executor:
            comparisons = {
                (directory, prediction["model_run_id"]): executor.submit(
                    _compare_structure_files,
                    directory / "model_runs" / prediction["model_run_id"] / "structure_report.json",
                    structures[(prediction["target_config"], prediction["workload_id"])],
                    diagnostics.keep_debug_artifacts,
                )
                for directory, prediction, _ in predictions
                if (prediction["target_config"], prediction["workload_id"]) in structures
            }
            structure_scores = {key: future.result() for key, future in comparisons.items()}

    rows = []
    source_windows = {}
    source_replays = {}
    for directory, prediction, run in predictions:
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
        model_output = directory / "model_runs" / prediction["model_run_id"]
        if structures:
            row["structure"] = structure_scores.get(
                (directory, prediction["model_run_id"]),
                {"status": "NOT_REQUESTED", "reason": "No target structure profile selected for this cell."},
            )

        source_report_path = model_output / "source_replay_report.json"
        if directory in experiment_report_directories:
            source_report = load_json(source_report_path)
            row["source_replay"] = {
                "status": source_report["status"],
                "report": str(source_report_path),
                "cost_basis": source_report["cost_basis"],
                "timing_basis": source_report["timing_basis"],
                "observed_e2e_us": source_report.get("observed_e2e_us"),
                "replayed_e2e_us": source_report.get("replayed_e2e_us"),
                "absolute_percentage_error": source_report.get("absolute_percentage_error"),
                "request_latency_accuracy": source_report.get("request_latency_accuracy"),
                "event_coverage": source_report["event_coverage"],
                "dependency_audit": source_report["dependency_audit"],
                "dependency_correctness_proven": source_report["dependency_correctness_proven"],
            }
            source_replays.setdefault(source_manifest, row["source_replay"])
        row["full_e2e"]["measurement_mode"] = "normal_http" if normal_http else "profiled_http"
        write_json(output / "cells" / f"{row['model_run_id']}.json", row)
        rows.append(row)
    cross, self_rows = [row for row in rows if not row["is_self"]], [row for row in rows if row["is_self"]]
    source_manifests = {row["source_manifest"] for row in rows}
    missing_source_reports = sorted(source_manifests - source_replays.keys())
    unavailable_source_replays = sorted(
        manifest for manifest, report in source_replays.items() if report["status"] != "reported"
    )
    source_report_status = "NOT_REQUESTED"
    if experiment_report_directories:
        source_report_status = (
            "REPORTED" if not missing_source_reports and not unavailable_source_replays else "NOT_FULLY_RECORDED"
        )

    by_source = {
        base: score_metrics([row for row in cross if row["source_config_id"] == base])
        for base in sorted({row["source_config_id"] for row in cross})
    }
    by_workload = {
        workload: score_metrics([row for row in cross if row["workload_id"] == workload])
        for workload in sorted({row["workload_id"] for row in cross})
    }
    by_target = {
        target: score_metrics([row for row in cross if row["target_config"] == target])
        for target in sorted({row["target_config"] for row in cross})
    }
    query_groups: dict[str, list[dict]] = {}
    for row in cross:
        query_groups.setdefault(cost_query_domain(row), []).append(row)

    result = {
        **score_metrics(cross),
        "by_source": by_source,
        "by_workload": by_workload,
        "by_target": by_target,
        "by_cost_query_domain": {
            "scope": "Recorded independent operator queries only; I/O payload models, migrated costs and retained gaps are not classified here.",
            "classification": "Any outside-range query takes precedence over an unseen combination within coordinate ranges; neither establishes whole-DAG cost coverage or confidence.",
            "groups": {name: score_metrics(group) for name, group in sorted(query_groups.items())},
        },
        "self_check": score_metrics(self_rows) if self_rows else None,
        "configuration_selection": configuration_selection(rows),
        "prediction_directories": [str(path) for path in prediction_dirs],
        "selected_target_configs": sorted(selected_targets) if selected_targets is not None else None,
        "predictions_completed": len(rows),
        "superseded_prediction_attempts": superseded,
        "new_predictions": 0,
        "new_acquisitions": 0,
        "target_profile_count": len(targets),
        "measurement_sample_count": sum(len(group) for group in samples.values()),
        "measurement_mode": "normal_http" if normal_http else "profiled_http",
        "evaluation_wall_seconds": time.monotonic() - started,
        "new_target_extractions": new_extractions + len(structures),
        "new_structure_extractions": len(structures),
        "new_oracle_observation_extractions": new_extractions,
        "target_observations_reused": len(observations) - new_extractions,
        "http_only_target_count": len(targets) - len(observations),
        "costs": {str(path): recorded_costs(path) for path in prediction_dirs},
        "target_opened_after_all_selected_predictions": True,
        "parameters_or_capture_plan_changed": False,
        "training_data_isolation": {
            "status": "NOT_RECORDED" if unrecorded_training else "HELD_OUT",
            "unrecorded_prediction_directories": unrecorded_training,
            "scope": "all selected bases and actual performance/structure evaluation captures",
            "limitations": "Checks declared training provenance, not hidden copied traces; historical predictions without provenance remain unverified.",
        },
        "structure_reports": {
            "enabled": bool(structures),
            "cell_count": len(structure_scores),
            "selected_target_count": len(structures),
            "unassessed_cell_count": len(rows) - len(structure_scores) if structures else None,
            "full_structure_exact_proven": False,
        },
        "stages": {
            "source_dag": {
                "status": source_report_status,
                "scope": "source replay with builder wait normalization against the same instrumented source; not normal HTTP accuracy or proof of all dependencies",
                "sources": [{"source_manifest": manifest, **report} for manifest, report in source_replays.items()],
                "missing_source_reports": missing_source_reports if experiment_report_directories else [],
                "unavailable_source_replays": unavailable_source_replays,
            },
            "target_structure": {
                "status": "PARTIAL_COVERAGE" if structures else "NOT_REQUESTED",
                "compared_cells": len(structure_scores),
                "full_structure_exact_proven": False,
                "details_retained": diagnostics.keep_debug_artifacts,
                "reports": [
                    "cells/" + row["model_run_id"] + ".json"
                    for row in rows
                    if (row["target_config"], row["workload_id"]) in structures
                ],
            },
            "performance": {
                "scope": "full HTTP formal window; gap, unattributed costs and Prefill/Decode are not excluded",
                "measurement_mode": "normal_http" if normal_http else "profiled_http",
                "metrics_field": "full_e2e",
                "accuracy_gates_field": "gates",
                "execution_does_not_imply_accuracy": True,
            },
        },
        "formal_and_owner_scope": "full HTTP formal window without exclusions; optional static oracle replay is diagnostic only",
    }
    if any(group["status"] != "PASS" for group in by_source.values()):
        result["status"] = "MODEL_LIMITATION"
    if oracle_cost_replay:
        from markov_internal.applications.accuracy.oracle_cost_replay.runner import run_suite

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
        "--target-config",
        action="append",
        default=[],
        help="Explicit target label subset for independent confirmation; default scores every cell.",
    )
    parser.add_argument(
        "--retry-prediction-dir",
        type=Path,
        action="append",
        default=[],
        help="Explicit retry of incomplete cells in --prediction-dir; cannot replace completed cells or change targets.",
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
        "--structure-profile-manifest",
        type=Path,
        action="append",
        default=[],
        help="Separate traced target for experimental structure comparison; repeat for each target/workload.",
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
        "--diagnostics",
        choices=tuple(level.value for level in DiagnosticLevel),
        default=DiagnosticLevel.OFF.value,
        help="Full retains per-operator structure mismatches; default keeps coverage and difference counts.",
    )
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
        structure_manifests=[require_repo_path(path) for path in args.structure_profile_manifest],
        diagnostics=DiagnosticLevel(args.diagnostics),
        retry_dirs=[require_repo_path(path) for path in args.retry_prediction_dir],
        selected_targets=set(args.target_config) if args.target_config else None,
    )
    if result["status"] == "NOT_READY":
        print(
            f"evaluation=NOT_READY completed={result['predictions_completed']}/{result['predictions_selected']} "
            "accuracy=not_scored new_predictions=0",
            flush=True,
        )
    else:
        print(f"evaluation={result['status']} cross={result['cell_count']} new_predictions=0", flush=True)
    replay_ready = all(
        item["ready_count"] == item["selected_cell_count"] for item in result.get("oracle_cost_replay", {}).values()
    )
    return 0 if result["status"] == "PASS" and replay_ready else 2


if __name__ == "__main__":
    raise SystemExit(main())
