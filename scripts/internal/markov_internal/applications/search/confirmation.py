"""Normal candidate screening and independent validation, outside the predictor."""

from __future__ import annotations

from collections import defaultdict
from dataclasses import asdict
from pathlib import Path
from typing import Any

from markov_internal.applications.search.request import SearchRequest
from markov_internal.common.commands import command_tokens, replace_command_option
from markov_internal.common.io import load_json, write_json
from markov_internal.common.manifest import profile_labels
from markov_internal.common.paths import ROOT_DIR, repo_relative_path, require_repo_path
from markov_internal.common.process import run_command
from markov_internal.modeling.deployment import cost_environment_matches, source_environment
from markov_internal.modeling.sglang_config import canonical_hicache, hicache_workload_spec
from markov_internal.modeling.source_dag.profiles import (
    ProfileRunRef,
    discover_profile_runs,
    extract_hicache_from_server_command,
)
from markov_internal.modeling.types import TargetHiCacheConfig
from markov_internal.preparation.capture import profile_usage
from markov_internal.preparation.profile_capture import (
    capture_profile_jobs,
    normal_confirmation_job,
    profile_capture_job,
    profile_token_plan,
)
from markov_internal.preparation.resources import wait_for_normal_resources
from markov_internal.profiling.suite import profile_experiments


def _measure(
    request: SearchRequest,
    state: dict[str, Any],
    selected: list[str],
    manifests: list[str],
    sources: dict[str, ProfileRunRef],
    output: Path,
) -> dict[str, Any]:
    """Score only the supplied phase's normal samples; never change predictions."""

    for normal in discover_profile_runs(tuple(require_repo_path(path) for path in manifests)):
        if normal.config_id not in selected or normal.input_id not in request.weights:
            continue
        if not cost_environment_matches(
            source_environment(normal, request.scenario.resource_environment),
            source_environment(sources[normal.input_id], request.scenario.resource_environment),
        ):
            raise ValueError("normal confirmation changed the fixed model, runtime or resources")

    evidence: dict[str, dict[str, tuple[dict[str, Any], str]]] = defaultdict(dict)
    for attempt in state["attempts"]:
        if attempt["report"] is None:
            continue
        for row in load_json(require_repo_path(attempt["report"]))["prediction"]["cells"]:
            if row["target_config"] in selected and row["status"] == "EXECUTED":
                evidence[row["target_config"]].setdefault(row["workload_id"], (row, attempt["prediction_dir"]))

    measurements = []
    for label in selected:
        target_output = output / f"target_{len(measurements) + 1}"
        command = [
            str(ROOT_DIR / "scripts/model.sh"),
            "evaluate",
            "--normal-http",
            "--target-config",
            label,
            "--output-dir",
            str(repo_relative_path(target_output)),
        ]
        for directory in dict.fromkeys(directory for _, directory in evidence[label].values()):
            command.extend(("--prediction-dir", directory))
        for manifest in manifests:
            command.extend(("--target-profile-manifest", manifest))
        code = run_command(command).returncode
        gate = load_json(target_output / "gate_summary.json")
        if gate["status"] in {"FAILED", "NOT_READY"}:
            return {
                "status": "EVALUATION_INCOMPLETE",
                "target_config": label,
                "gate": str(repo_relative_path(target_output / "gate_summary.json")),
                "return_code": code,
            }

        workloads = {
            workload: load_json(target_output / "cells" / (row["model_run_id"] + ".json"))["full_e2e"]
            for workload, (row, _) in evidence[label].items()
        }
        measured = sum(request.weights[name] * workloads[name]["target_us"] / 1e6 for name in request.weights)
        observed_range = [
            sum(request.weights[name] * workloads[name]["target_range_us"][i] / 1e6 for name in request.weights)
            for i in (0, 1)
        ]
        measurements.append(
            {
                "target_config": label,
                "measured_score_sec": measured,
                "workloads": workloads,
                "observed_weighted_range_sec": observed_range,
                "accuracy_status": gate["status"],
                "report": str(repo_relative_path(target_output / "gate_summary.json")),
            }
        )

    reference = state.get("reference_target", state["base_target"])["name"]
    baseline = next(row["measured_score_sec"] for row in measurements if row["target_config"] == reference)
    source_wall = sum(
        request.weights[name] * measurements[0]["workloads"][name]["base_us"] / 1e6 for name in request.weights
    )
    for row in measurements:
        row["measured_time_reduction_vs_reference"] = 1 - row["measured_score_sec"] / baseline
        row["measured_time_reduction_vs_base"] = 1 - row["measured_score_sec"] / source_wall
    return {
        "status": "MEASURED",
        "measurements": measurements,
        "scope": "All declared normal samples, full HTTP window; observed ranges are not confidence intervals.",
    }


def _automatic(
    request: SearchRequest,
    state: dict[str, Any],
    selection: dict[str, Any],
    targets: dict[str, dict[str, Any]],
    sources: dict[str, ProfileRunRef],
    output: Path,
) -> dict[str, Any]:
    """Screen once, then validate the selected candidate and reference with fresh samples."""

    selected = selection["targets"]
    specs = output / "normal_configs.json"
    write_json(specs, {"configs": [hicache_workload_spec(name, targets[name]) for name in selected]})
    plans = {}
    ascend = any("ASCEND_VISIBLE_DEVICES" in source_environment(source)["env"] for source in sources.values())
    for workload, source in sources.items():
        plan = profile_token_plan(source, request.scenario.forced_token_bundle)
        if plan is None:
            raise ValueError("automatic confirmation requires the source's recorded token replay plan")
        plans[workload] = plan

    def capture(labels: list[str], phase: str, repetitions: int) -> dict[str, Any]:
        jobs = []
        for repetition in range(repetitions):
            rotated = labels[repetition % len(labels) :] + labels[: repetition % len(labels)]
            for workload in request.weights:
                for label in rotated:
                    jobs.append(
                        normal_confirmation_job(
                            sources[workload],
                            TargetHiCacheConfig(label, targets[label]),
                            output,
                            specs,
                            plans[workload],
                            repetition=repetition + 1,
                            index=len(jobs) + 1,
                            phase=phase,
                        )
                    )
        manifests = []
        failed = {}
        ledger_path = output / "confirmation_capture_ledger.json"
        ledger = str(repo_relative_path(ledger_path))
        history = load_json(ledger_path)["attempts"] if ledger_path.exists() else []
        result = {"usage": profile_usage(history), "budget": asdict(request.confirmation_budget)}
        for row in history:
            metadata = row["inputs"]["suite"]["metadata"]
            if row["status"] == "failed" and metadata.get("confirmation_phase") == phase:
                failed.setdefault(
                    metadata["config_id"],
                    {
                        "workload": row["workload"],
                        "profile_manifest": row.get("profile_manifest"),
                        "capture_ledger": ledger,
                    },
                )

        for job in jobs:
            if job.config_id in failed:
                continue

            result = capture_profile_jobs(
                [job], output, request.confirmation_budget, dry_run=True, purpose="confirmation"
            )
            if result["status"] != "profiles_captured":
                if ascend:
                    wait_for_normal_resources(output)
                result = capture_profile_jobs(
                    [job], output, request.confirmation_budget, dry_run=False, purpose="confirmation"
                )
            if result["status"] == "failed":
                failed[job.config_id] = {
                    "workload": job.workload,
                    "profile_manifest": result["attempt"].get("profile_manifest"),
                    "capture_ledger": ledger,
                }
                continue
            if result["status"] != "profiles_captured":
                return {
                    "status": result["status"],
                    "stop_reason": result.get("stop_reason"),
                    "limits": result.get("limits"),
                    "capture_ledger": ledger,
                    "failed_targets": failed,
                }
            manifests.extend(result["profile_manifests"])

        return {
            "status": "completed_with_failures" if failed else "profiles_captured",
            "profile_manifests": manifests,
            "failed_targets": failed,
            "capture_ledger": ledger,
            "usage": result["usage"],
            "budget": result["budget"],
        }

    screening = capture(selected, "screening", request.confirmation.get("screening_repetitions", 1))
    if screening["status"] not in {"profiles_captured", "completed_with_failures"}:
        return {"status": "MEASUREMENT_INCOMPLETE", "phase": "screening", "capture": screening}
    reference = selection["reference"]
    anchor = state["base_target"]["name"]
    if {reference, anchor} & screening["failed_targets"].keys():
        return {"status": "MEASUREMENT_INCOMPLETE", "phase": "screening", "capture": screening}
    screen = _measure(
        request,
        state,
        [label for label in selected if label not in screening["failed_targets"]],
        screening["profile_manifests"],
        sources,
        output / "screening",
    )
    screen["failed_targets"] = screening["failed_targets"]
    write_json(output / "screening_summary.json", screen)
    if screen["status"] != "MEASURED":
        return screen

    # An out-of-space source anchor is measured for scoring, not eligible for deployment.
    eligible = set(state["ranked_candidates"])
    winner = min(
        (row for row in screen["measurements"] if row["target_config"] in eligible),
        key=lambda row: (row["measured_score_sec"], selected.index(row["target_config"])),
    )["target_config"]
    final_targets = list(dict.fromkeys([reference, winner]))
    write_json(
        output / "validation_selection.json",
        {
            "targets": final_targets,
            "screened_choice": winner,
            "model_first": selection["model_first"],
            "rule": "Selection uses screening only; validation samples are fresh and never fed to search or training.",
        },
    )
    validation = capture(final_targets, "validation", request.confirmation.get("repetitions", 5))
    if (
        validation["status"] not in {"profiles_captured", "completed_with_failures"}
        or reference in validation["failed_targets"]
    ):
        return {"status": "MEASUREMENT_INCOMPLETE", "phase": "validation", "capture": validation}

    manifests = list(validation["profile_manifests"])
    valid_targets = [label for label in final_targets if label not in validation["failed_targets"]]
    if anchor not in valid_targets:
        # The evaluator still needs normal source windows; reuse only the source anchor,
        # never a target screening sample in a final target's five-sample mean.
        manifests.extend(
            str(repo_relative_path(source.manifest_path))
            for source in discover_profile_runs(tuple(require_repo_path(p) for p in screening["profile_manifests"]))
            if source.config_id == anchor
        )
    final = _measure(request, state, valid_targets, manifests, sources, output / "validation")
    if final["status"] != "MEASURED":
        return final
    control = final["measurements"][0]
    chosen = next((row for row in final["measurements"] if row["target_config"] == winner), None)
    separated = (
        chosen is not None
        and winner != reference
        and chosen["observed_weighted_range_sec"][1] < control["observed_weighted_range_sec"][0]
    )
    recommended_path = output / "recommended_config.json"
    if separated:
        write_json(recommended_path, {"name": winner, "hicache": targets[winner]})
    return {
        "status": "CONFIRMED_IMPROVEMENT" if separated else "NO_CONFIRMED_IMPROVEMENT",
        "selection": selection,
        "screening": screen,
        "screened_choice": winner,
        "measurements": final["measurements"],
        "capture": validation,
        "recommended_target": winner if separated else None,
        "recommended_config": str(repo_relative_path(recommended_path)) if separated else None,
        "scope": "Fresh validation of selected candidates only; disjoint sample ranges are not confidence intervals.",
        "fallback": None
        if separated
        else "Keep the validated same-resource reference when it is the starting deployment; otherwise no verified recommendation.",
    }


def confirm(request: SearchRequest, state: dict[str, Any]) -> dict[str, Any]:
    """Write model selection before reading any target normal observations."""

    reference_target = state.get("reference_target", state["base_target"])
    reference = reference_target["name"]
    reference_score = state.get("same_resource_reference", state["baseline"])["score_sec"]
    if not state.get("ranked_candidates") or reference_score is None:
        return {"status": "NOT_RUN", "reason": "complete candidate and reference predictions are required"}

    options = request.confirmation
    automatic = not options.get("profile_suites") and not options.get("target_profile_manifests")
    if automatic and request.space.locate(reference_target["hicache"]) is None:
        return {
            "status": "REFERENCE_REQUIRED",
            "reason": "Provide reference_config inside the declared resource space.",
        }
    ranked = state["ranked_candidates"]
    count = options.get("top_count", 1)
    predictions = {row["name"]: row["score_sec"] for row in state["candidates"]}
    targets = {row["name"]: row["hicache"] for row in state["selected_candidates"]}
    targets[state["base_target"]["name"]] = state["base_target"]["hicache"]
    representatives = ranked[:count]
    remaining = ranked[count:]
    competitors = []
    if automatic:
        mechanisms = {
            (targets[name]["page_size"], targets[name]["write_policy"], targets[name]["prefetch_policy"])
            for name in representatives
        }
        for name in remaining:
            mechanism = (targets[name]["page_size"], targets[name]["write_policy"], targets[name]["prefetch_policy"])
            if mechanism not in mechanisms:
                competitors.append(name)
                mechanisms.add(mechanism)
            if len(competitors) == 2:
                break
    else:
        competitor = next(
            (name for name in remaining if predictions[name] > predictions[representatives[-1]]),
            remaining[0] if remaining else None,
        )
        competitors = [competitor] if competitor else []
    fixed = [
        row["name"]
        for row in state["selected_candidates"]
        if row["space_index"] in request.initial_indices and row["name"] in ranked
    ]
    anchors = [state["base_target"]["name"], reference]

    selected = list(dict.fromkeys([*anchors, *fixed, *representatives, *competitors]))
    output = request.output_dir / "confirmation"
    (output / "recommended_config.json").unlink(missing_ok=True)
    selection = {
        "targets": selected,
        "reference": reference,
        "model_first": ranked[0],
        "weights": request.weights,
        "rule": "Source anchor for scoring, same-resource reference, declared initial candidates, top representatives and mechanism-diverse alternatives."
        if automatic
        else "Base, reference, top representatives and a score-distinct competitor.",
        "scope": "Preselected subset only, not real global optimality; prediction ties remain ties.",
    }
    write_json(output / "selection.json", selection)
    sources = {
        source.input_id: source
        for source in discover_profile_runs(
            tuple(require_repo_path(p) for p in state["prepared_inputs"]["source_manifests"])
        )
    }
    if automatic:
        result = _automatic(request, state, selection, targets, sources, output)
        write_json(output / "confirmation_summary.json", result)
        return result

    jobs = []
    for path in options.get("profile_suites", []):
        for experiment in profile_experiments(load_json(require_repo_path(path))):
            label, workload = profile_labels(experiment)
            if label not in selected or workload not in request.weights:
                continue
            profiling = experiment["profiling"]
            if (
                profiling["enabled"] is not False
                or profiling["channels"]
                or any(
                    "TIMING" in key and not (key.endswith(("_TIMING", "_TIMING_EMISSION")) and str(value) == "0")
                    for key, value in experiment.get("env", {}).items()
                )
            ):
                raise ValueError("confirmation suites must disable profiling and timing instrumentation")
            actual = extract_hicache_from_server_command(experiment["server"]["command"])
            if actual is None or canonical_hicache(actual) != canonical_hicache(targets[label]):
                raise ValueError(f"confirmation launch does not match predicted HiCache parameters: {label}")
            plan = profile_token_plan(sources[workload], request.scenario.forced_token_bundle)
            if plan is None:
                raise ValueError("normal confirmation requires the selected source's token replay plan")
            command = command_tokens(experiment["bench"]["command"])
            replace_command_option(command, "--forced-token-mode", "replay")
            replace_command_option(command, "--forced-token-plan", str(repo_relative_path(plan)))
            experiment["bench"]["command"] = command
            experiment.setdefault("metadata", {})["confirmation_repetition"] = len(jobs)
            jobs.append(
                profile_capture_job(
                    experiment,
                    len(jobs) + 1,
                    config_id=label,
                    workload=workload,
                    purpose="confirmation",
                )
            )

    manifests = list(options.get("target_profile_manifests", []))
    captured = None
    if jobs:
        captured = capture_profile_jobs(
            jobs, output, request.confirmation_budget, dry_run=False, purpose="confirmation"
        )
        if captured["status"] != "profiles_captured":
            return {"status": "MEASUREMENT_INCOMPLETE", "selection": selection, "capture": captured}
        manifests.extend(captured["profile_manifests"])
    result = _measure(request, state, selected, manifests, sources, output)
    result.update(selection=selection, capture=captured)
    if result["status"] == "MEASURED":
        best = min(row["measured_score_sec"] for row in result["measurements"])
        result["measured_best_targets"] = [
            row["target_config"] for row in result["measurements"] if row["measured_score_sec"] == best
        ]
    write_json(output / "confirmation_summary.json", result)
    return result
