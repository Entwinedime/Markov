"""Post-selection prediction completion, reference and control experiments."""

from __future__ import annotations

import argparse
from collections import defaultdict
from dataclasses import replace
from pathlib import Path
import time
from typing import Any

from examples.hicache_search.run import normal, normal_jobs, public_command
from markov_internal.applications.search.confirmation import _measure
from markov_internal.applications.search.request import SearchRequest
from markov_internal.applications.search.results import prediction_rows
from markov_internal.common.io import load_json, write_json, write_json_atomic
from markov_internal.common.paths import repo_relative_path, require_repo_path
from markov_internal.modeling.context import PredictionInputs
from markov_internal.modeling.execution.results import summarize
from markov_internal.modeling.invoke import invoke_prediction
from markov_internal.modeling.sglang_config import canonical_hicache
from markov_internal.modeling.source_dag.profiles import cpu_service_metadata, discover_profile_runs
from markov_internal.modeling.types import TargetHiCacheConfig
from markov_internal.preparation.capture import CAPTURE_CLEANUP_RESERVE_SEC, profile_usage


def normal_allowance(root: Path, protocol: dict[str, Any]) -> dict[str, float | int]:
    """Subtract actual formal captures, including failures; development is separately budgeted."""

    paths = [
        root / "finite_reference/normal/confirmation_capture_ledger.json",
        root / "blackbox/confirmation_capture_ledger.json",
    ]
    for task in protocol["final_tasks"]:
        request = load_json(require_repo_path(task["request"]))
        paths.append(require_repo_path(request["output_dir"]) / "confirmation/confirmation_capture_ledger.json")
        paths.append(
            root / "controls" / f"{task['family']}_{task['resource']}" / "normal/confirmation_capture_ledger.json"
        )
    paths.extend(
        require_repo_path(path) / "normal/confirmation_capture_ledger.json"
        for path in protocol.get("confirmation_recoveries", {}).values()
    )

    remaining = dict(protocol["normal_plan"]["budget"])
    for path in paths:
        if path.exists():
            usage = profile_usage(load_json(path)["attempts"])
            for key in remaining:
                remaining[key] -= usage[key]
    return remaining


def capture_group(root: Path, group: Path, protocol: dict[str, Any]) -> None:
    """Admit one fixed capture pass within the remaining formal aggregate allowance."""

    selection = load_json(group / "selection.json")
    jobs = normal_jobs(group, selection)
    remaining = normal_allowance(root, protocol)
    ledger = group / "normal/confirmation_capture_ledger.json"
    history = load_json(ledger)["attempts"] if ledger.exists() else []
    pending = [
        job
        for job in jobs
        if not any(row["inputs"] == job.inputs and row["status"] in {"completed", "failed"} for row in history)
    ]
    needed = {
        "server_starts": len(pending),
        "requests": sum(job.counts["requests"] for job in pending),
        "tokens": sum(job.counts["tokens"] for job in pending),
    }
    if any(needed[key] > remaining[key] for key in needed) or (
        pending and remaining["wall_seconds"] <= CAPTURE_CLEANUP_RESERVE_SEC
    ):
        raise RuntimeError("Formal aggregate capture budget cannot cover this group; retain usage and inspect")

    declaration = load_json(group / "protocol.json")
    declaration["budget"] = {
        "normal_server_starts": len(jobs),
        "normal_wall_seconds": remaining["wall_seconds"] + profile_usage(history)["wall_seconds"],
    }
    write_json(group / "protocol.json", declaration)
    normal(group, dry_run=False, keep_failures=True)


def task_state(task: dict[str, Any], protocol: dict[str, Any]) -> dict[str, Any]:
    """Use an explicitly declared independent recovery without overwriting the original failure."""

    request = load_json(require_repo_path(task["request"]))
    state = load_json(require_repo_path(request["output_dir"]) / "search_summary.json")
    recovery = protocol.get("confirmation_recoveries", {}).get(f"{task['family']}_{task['resource']}")
    if recovery is not None:
        state["confirmation"] = load_json(require_repo_path(recovery) / "confirmation_summary.json")
    return state


def recover_confirmations(root: Path, protocol: dict[str, Any]) -> None:
    """Collect one fresh pass for an already selected pair after zero-request startup failures."""

    for task in protocol["final_tasks"]:
        name = f"{task['family']}_{task['resource']}"
        if name not in protocol["confirmation_recoveries"]:
            continue

        request = SearchRequest.load(require_repo_path(task["request"]))
        state = load_json(request.output_dir / "search_summary.json")
        original = request.output_dir / "confirmation"
        selected = load_json(original / "validation_selection.json")
        group = require_repo_path(protocol["confirmation_recoveries"][name])
        targets = {row["name"]: row for row in [*state["selected_candidates"], state["reference_target"]]}
        workload = next(iter(request.weights))
        write_json(
            group / "selection.json",
            {
                "workloads": {
                    workload: {
                        "source": {"manifest": state["prepared_inputs"]["source_manifests"][0]},
                        "targets": [targets[label] for label in selected["targets"]],
                    }
                },
                "rule": {"repetitions": 5},
                "original_selection": str(repo_relative_path(original / "validation_selection.json")),
            },
        )
        write_json(
            group / "protocol.json",
            {"normal_confirmation": {"repetitions": 5}, "forced_token_bundle": request.scenario.forced_token_bundle},
        )
        capture_group(root, group, protocol)
        ledger = group / "normal/confirmation_capture_ledger.json"
        attempts = load_json(ledger)["attempts"]
        capture = {
            "profile_manifests": [row["profile_manifest"] for row in attempts if row["status"] == "completed"],
            "capture_ledger": str(repo_relative_path(ledger)),
            "usage": profile_usage(attempts),
        }
        result = {"status": "MEASUREMENT_INCOMPLETE", "capture": capture}
        if len(attempts) == 10 and all(row["status"] == "completed" for row in attempts):
            sources = {
                source.input_id: source
                for source in discover_profile_runs(
                    tuple(require_repo_path(p) for p in state["prepared_inputs"]["source_manifests"])
                )
            }
            screening = load_json(original / "confirmation_capture_ledger.json")["attempts"]
            anchors = [
                row["profile_manifest"]
                for row in screening
                if row["status"] == "completed"
                and row["inputs"]["suite"]["metadata"]["config_id"] == state["base_target"]["name"]
            ]
            result = _measure(
                request,
                state,
                selected["targets"],
                [*capture["profile_manifests"], *anchors],
                sources,
                group / "evaluation",
            )
            if result["status"] == "MEASURED":
                reference, chosen = result["measurements"]
                separated = chosen["observed_weighted_range_sec"][1] < reference["observed_weighted_range_sec"][0]
                recommended = group / "recommended_config.json"
                if separated:
                    target = targets[selected["screened_choice"]]
                    write_json(recommended, {"name": target["name"], "hicache": target["hicache"]})

                result.update(
                    status="CONFIRMED_IMPROVEMENT" if separated else "NO_CONFIRMED_IMPROVEMENT",
                    selection=load_json(original / "selection.json"),
                    screened_choice=selected["screened_choice"],
                    capture=capture,
                    recommended_target=selected["screened_choice"] if separated else None,
                    recommended_config=str(repo_relative_path(recommended)) if separated else None,
                    fallback=None if separated else "Keep the validated same-resource reference; no verified change.",
                )
        result["original_incomplete_confirmation"] = str(repo_relative_path(original / "confirmation_summary.json"))
        result["scope"] = (
            "One fresh five-run pair; prior zero-request failures and primary selection retained. No refit."
        )
        write_json(group / "confirmation_summary.json", result)
        if result["status"] not in {"CONFIRMED_IMPROVEMENT", "NO_CONFIRMED_IMPROVEMENT"}:
            raise RuntimeError(f"Independent recovery is incomplete; retain failed attempts: {group}")


def _unfinished_prediction(row: dict[str, Any] | None) -> bool:
    return row is None or (
        row["status"] != "EXECUTED" and set(row["blockers"]) <= {"not_started", "not_started_after_failure"}
    )


def timeout_targets(state: dict[str, Any]) -> dict[str, list[TargetHiCacheConfig]]:
    """Select unfinished cells from timed-out primary batches, never semantic failures or new candidates."""

    rows = prediction_rows(state)
    last_attempt = {
        (name, attempt["workload"]): attempt["return_code"]
        for attempt in state["attempts"]
        for name in attempt["targets"]
    }
    targets = {
        target["name"]: target["hicache"]
        for target in [*state["selected_candidates"], state["base_target"], state["reference_target"]]
    }
    pending = defaultdict(list)
    for (name, workload), code in last_attempt.items():
        row = rows.get((name, workload))
        if code == 124 and _unfinished_prediction(row):
            pending[workload].append(TargetHiCacheConfig.from_raw({"name": name, "hicache": targets[name]}))

    return dict(pending)


def complete_predictions(root: Path, protocol: dict[str, Any]) -> None:
    """Finish original timeout cells after confirmation; keep primary decisions and budgets untouched."""

    results = []
    for task in protocol["final_tasks"]:
        request = load_json(require_repo_path(task["request"]))
        primary = require_repo_path(request["output_dir"]) / "search_summary.json"
        state = load_json(primary)
        output = root / "prediction_completion" / f"{task['family']}_{task['resource']}"
        path = output / "completion_summary.json"
        history = load_json(path) if path.exists() else {"attempts": []}
        if any(attempt["status"] == "running" for attempt in history["attempts"]):
            raise RuntimeError(f"Unsettled prediction completion; inspect its process before resuming: {path}")

        selected = timeout_targets(state)
        prepared = state["prepared_inputs"]
        inputs = PredictionInputs(
            source_manifests=tuple(require_repo_path(p) for p in prepared["source_manifests"]),
            cpu_service_costs=tuple(require_repo_path(p) for p in prepared["cpu_service_costs"]),
            cost_model=require_repo_path(prepared["cost_model"]),
            resource_environment=prepared["resource_environment"],
        )
        sources = {source.input_id: source for source in discover_profile_runs(inputs.source_manifests)}
        cpu = {
            require_repo_path(cpu_service_metadata(p)["source_manifest"]).resolve(): p for p in inputs.cpu_service_costs
        }
        combined = {"attempts": [*state["attempts"], *history["attempts"]]}
        rows = prediction_rows(combined)
        for workload, original_targets in selected.items():
            targets = [
                target for target in original_targets if _unfinished_prediction(rows.get((target.label, workload)))
            ]
            if not targets:
                continue

            source = sources[workload]
            service = cpu.get(source.manifest_path.resolve())
            subset = replace(
                inputs, source_manifests=(source.manifest_path,), cpu_service_costs=(service,) if service else ()
            )
            directory = output / f"attempt_{len(history['attempts']) + 1}"
            attempt = {
                "status": "running",
                "workload": workload,
                "targets": [target.label for target in targets],
                "scheduled_predictions": len(targets),
                "charged_predictions": len(targets),
                "report": None,
                "prediction_dir": str(repo_relative_path(directory)),
                "wall_seconds": None,
            }
            history["attempts"].append(attempt)
            history["status"] = "predicting"
            write_json_atomic(path, history)
            outcome = invoke_prediction(
                subset,
                tuple(targets),
                directory,
                jobs=protocol["prediction_completion"]["model_run_jobs"],
                trace_threads=request["execution"]["trace_threads"],
                trace_file_threads=request["execution"]["trace_file_threads"],
                diagnostics=request["execution"]["diagnostics"],
                max_wall_sec=protocol["prediction_completion"]["safety_timeout_sec"],
            )
            attempt.update(
                status="finished",
                return_code=outcome.return_code,
                wall_seconds=outcome.wall_seconds,
                charged_predictions=outcome.charged_predictions(len(targets)),
                report=str(repo_relative_path(outcome.report_path)) if outcome.summary is not None else None,
            )
            history["status"] = "interrupted" if outcome.return_code not in {0, 2} else "predicting"
            write_json_atomic(path, history)
            if outcome.return_code not in {0, 2}:
                raise RuntimeError(
                    f"Prediction completion exited {outcome.return_code}; inspect retained results: {path}"
                )

        combined["attempts"] = [*state["attempts"], *history["attempts"]]
        rows = prediction_rows(combined)
        requested = [(target.label, workload) for workload, targets in selected.items() for target in targets]
        unfinished = [key for key in requested if _unfinished_prediction(rows.get(key))]
        failures = [key for key in requested if key not in unfinished and rows[key]["status"] != "EXECUTED"]
        status = "completed_with_prediction_failures" if failures else "completed"
        if unfinished:
            status = "incomplete"

        history.update(
            status=status,
            original_search_summary=str(repo_relative_path(primary)),
            original_timeout_cells=len(requested),
            completed_timeout_cells=sum(rows[key]["status"] == "EXECUTED" for key in requested if key in rows),
            unfinished_timeout_cells=unfinished,
            failed_timeout_cells=failures,
            additional_charged_predictions=sum(a["charged_predictions"] for a in history["attempts"]),
            additional_wall_seconds=sum(a["wall_seconds"] for a in history["attempts"]),
            prediction=summarize(list(rows.values())),
            scope="Original selected cells only; primary ranking and confirmation unchanged. Extra work is charged separately.",
        )
        write_json_atomic(path, history)
        results.append(
            {
                "task": f"{task['family']}_{task['resource']}",
                "summary": str(repo_relative_path(path)),
            }
        )
        write_json(root / "prediction_completion/completion_summary.json", {"tasks": results})
        if unfinished:
            raise RuntimeError(f"Prediction completion is interrupted; inspect retained results: {path}")


def reference(root: Path, protocol: dict[str, Any]) -> None:
    complete_predictions(root, protocol)
    declaration = load_json(root / "inputs/finite_reference_prediction.json")
    # Direct prediction calls need the same container naming as the memory watchdog.
    command = [
        "env",
        f"TRACE_SIM_RUN_CONTAINER_NAME=markov-prediction-reference-{time.time_ns()}",
        *declaration["command"],
    ]
    public_command(root, "finite_reference_prediction", command, accepted=(0, 2))
    cells = load_json(root / "finite_reference/prediction/workflow_summary.json")["prediction"]["cells"]
    valid = [row for row in cells if row["status"] == "EXECUTED"]
    best = min(valid, key=lambda row: row["http_e2e_us"]) if valid else None
    write_json(
        root / "finite_reference/model_selection.json",
        {
            "selected_before_reference_measurements_at_unix": time.time(),
            "selected_target": best["target_config"] if best else None,
            "predicted_http_e2e_sec": best["http_e2e_us"] / 1e6 if best else None,
            "successful_predictions": len(valid),
            "attempted_predictions": len(cells),
            "scope": "Restricted model choice, separate from the full-space search; no measured table used.",
        },
    )
    capture_group(root, root / "finite_reference", protocol)

    request = load_json(root / "inputs/final/competitive_revisit_tight.json")
    remaining = normal_allowance(root, protocol)
    # The black-box baseline keeps its own capture ledger and never calls the predictor.
    for key, value in request["confirmation"]["budget"].items():
        request["confirmation"]["budget"][key] = min(value, remaining[key])
    path = root / "inputs/live_smac.json"
    write_json(path, request)
    public_command(
        root,
        "live_smac",
        [
            "build/research/smac/bin/python",
            "examples/hicache_search/blackbox.py",
            "--request",
            str(repo_relative_path(path)),
            "--configspace",
            str(repo_relative_path(root / "inputs/smac_configspace.json")),
            "--output-dir",
            str(repo_relative_path(root / "blackbox")),
            "--trials",
            "6",
        ],
        accepted=(0, 2),
    )


def control_plan(
    state: dict[str, Any], controls: dict[str, dict[str, Any]]
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    """Deduplicate control policies and reuse only complete fresh main-validation samples."""

    profiles = defaultdict(list)
    for source in discover_profile_runs(
        tuple(require_repo_path(path) for path in state["confirmation"]["capture"]["profile_manifests"])
    ):
        profiles[source.config_id].append(str(repo_relative_path(source.manifest_path)))
    fields = {row["name"]: row["hicache"] for row in state["selected_candidates"]}
    for key in ("base_target", "reference_target"):
        fields[state[key]["name"]] = state[key]["hicache"]

    first = state["ranked_candidates"][0]
    controls = {**controls, "pure_model": {"name": first, "hicache": fields[first]}}
    roles = []
    pending = {}
    for role, target in controls.items():
        canonical = canonical_hicache(target["hicache"])
        shared = next(
            (
                name
                for name, paths in profiles.items()
                if len(paths) == 5 and canonical_hicache(fields[name]) == canonical
            ),
            None,
        )
        row = {"role": role, **target, "reused_main_validation_manifests": profiles[shared] if shared else []}
        if shared is None:
            pending.setdefault(canonical, target)
            row["capture_target"] = pending[canonical]["name"]
        roles.append(row)
    return roles, list(pending.values())


def controls(root: Path, protocol: dict[str, Any]) -> None:
    fixed = load_json(root / "fixed_baselines/fixed_baselines.json")
    results = []
    for task in protocol["final_tasks"]:
        request = load_json(require_repo_path(task["request"]))
        state = task_state(task, protocol)
        policies = {
            role: load_json(require_repo_path(fixed["selected"][task["resource"]][role]))
            for role in ("best_effort", "development_fixed")
        }
        roles, pending = control_plan(state, policies)
        group = root / "controls" / f"{task['family']}_{task['resource']}"
        workload = next(iter(state["confirmation"]["selection"]["weights"]))
        write_json(group / "control_plan.json", {"roles": roles, "request": task["request"]})
        write_json(
            group / "selection.json",
            {
                "workloads": {
                    workload: {
                        "source": {"manifest": state["prepared_inputs"]["source_manifests"][0]},
                        "targets": pending,
                    }
                },
                "rule": {"repetitions": 5},
            },
        )
        write_json(
            group / "protocol.json",
            {
                "normal_confirmation": {"repetitions": 5},
                "forced_token_bundle": load_json(require_repo_path(request["scenario"]))["forced_token_bundle"],
            },
        )
        if pending:
            capture_group(root, group, protocol)
        results.append(
            {
                "family": task["family"],
                "resource": task["resource"],
                "new_distinct_targets": len(pending),
                "roles_reusing_main_validation": sum(bool(row["reused_main_validation_manifests"]) for row in roles),
                "plan": str(repo_relative_path(group / "control_plan.json")),
            }
        )
        write_json(
            root / "controls/capture_summary.json",
            {"tasks": results, "scope": "Failures retained; partial samples are not five-run confirmation means."},
        )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phase", choices=("recover-confirmations", "complete-predictions", "reference", "controls"))
    parser.add_argument("--root", type=Path, required=True)
    args = parser.parse_args()
    root = require_repo_path(args.root)
    protocol = load_json(root / "protocol.json")
    if args.phase == "recover-confirmations":
        recover_confirmations(root, protocol)
        return

    for task in protocol["final_tasks"]:
        state = task_state(task, protocol)
        confirmation = state.get("confirmation")
        if confirmation is None or confirmation["status"] not in {"CONFIRMED_IMPROVEMENT", "NO_CONFIRMED_IMPROVEMENT"}:
            parser.error("Main task independent confirmation is incomplete; post-selection experiments must wait")
    if args.phase == "complete-predictions":
        complete_predictions(root, protocol)
    elif args.phase == "reference":
        reference(root, protocol)
    else:
        controls(root, protocol)


if __name__ == "__main__":
    main()
