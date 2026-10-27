"""Matrix application: predict all declared groups, then independently score."""

from __future__ import annotations

import argparse
from pathlib import Path
import time
from typing import Any

from markov_internal.applications.prediction import load_targets, prepare
from markov_internal.common.io import load_json, write_json
from markov_internal.common.naming import sanitize
from markov_internal.common.paths import ROOT_DIR, repo_relative_path, require_repo_path
from markov_internal.common.process import run_command
from markov_internal.preparation.scenario import ScenarioRequest


def run_experiment(
    path: Path, *, dry_run: bool = False, predict_only: bool = False, score_only: bool = False
) -> dict[str, Any]:
    """Run a matrix recipe through the same model used by direct prediction.

    Group files contain predictor inputs only. The separate evaluation section
    is not opened by prediction; normal suites and target manifests are consumed
    only after every selected predictor has completed. Score-only never prepares.
    """

    request = load_json(path)
    allowed = {"groups", "prediction_dirs", "execution", "evaluation", "output_dir"}
    groups = request.get("groups", [])
    if set(request) - allowed:
        raise ValueError("unknown experiment inputs")
    if not score_only and (not groups or "prediction_dirs" in request):
        raise ValueError("prediction needs groups; explicit prediction_dirs are for --score-only")
    output = require_repo_path(request["output_dir"])
    execution = request.get("execution", {})
    if set(execution) - {"model_run_jobs", "trace_threads", "trace_file_threads", "diagnostics", "experiment_reports"}:
        raise ValueError("unknown experiment execution options")
    evaluation = request.get("evaluation", {})
    allowed_evaluation = {
        "normal_http",
        "target_profile_manifests",
        "profile_run_dirs",
        "structure_profile_manifests",
        "normal_suites",
        "oracle_cost_replay",
        "oracle_max_runs",
    }
    if set(evaluation) - allowed_evaluation:
        raise ValueError("unknown experiment evaluation options")

    started = time.monotonic()
    prediction_dirs = (
        [require_repo_path(path) for path in request["prediction_dirs"]]
        if score_only and "prediction_dirs" in request
        else [output / "groups" / str(index) / "predictions" for index in range(1, len(groups) + 1)]
    )
    if not prediction_dirs:
        raise ValueError("select at least one prediction group")
    summary: dict[str, Any] = {
        "status": "planning",
        "groups": [],
        "group_count": len(prediction_dirs),
        "evaluation": None,
    }
    report = output / "experiment_summary.json"
    write_json(report, summary)
    try:
        if not score_only:
            for index, source in enumerate(groups, start=1):
                raw = load_json(require_repo_path(source))
                raw["output_dir"] = str(repo_relative_path(output / "groups" / str(index)))
                group_path = output / "inputs" / f"group_{index}.json"
                write_json(group_path, raw)
                scenario = ScenarioRequest.load(group_path)
                summary["status"] = "predicting"
                write_json(report, summary)
                result = prepare(scenario, load_targets(scenario), dry_run=dry_run, **execution)
                summary["groups"].append(
                    {
                        "request": str(repo_relative_path(group_path)),
                        "status": result["status"],
                        "predictions_completed": result["predictions_completed"],
                        "prediction_count": result["prediction_count"],
                        "prediction_dir": str(repo_relative_path(prediction_dirs[index - 1])),
                    }
                )
                if result["status"] not in {"planned", "predicted"}:
                    summary["status"] = "prediction_incomplete"
                    return summary

        if dry_run or predict_only or "evaluation" not in request:
            summary["status"] = "planned" if dry_run else "predicted"
            return summary

        # The evaluator also enforces this before opening targets. Check here
        # before launching optional normal suites, especially on score-only.
        for directory in prediction_dirs:
            prediction = load_json(directory / "workflow_summary.json")["prediction"]
            if prediction["status"] != "EXECUTED":
                summary["status"] = "prediction_incomplete"
                return summary

        targets = list(evaluation.get("target_profile_manifests", []))
        if not score_only:
            for suite_path in evaluation.get("normal_suites", []):
                suite_path = require_repo_path(suite_path)
                suite = load_json(suite_path)
                if not evaluation.get("normal_http", False):
                    raise ValueError("normal_suites require normal_http evaluation")
                if suite["profiling"]["enabled"]:
                    raise ValueError("normal_suites must explicitly disable profiling")
                summary["status"] = "measuring_normal_http"
                write_json(report, summary)
                if run_command([str(ROOT_DIR / "scripts/profile.sh"), str(repo_relative_path(suite_path))]).returncode:
                    summary["status"] = "normal_measurement_failed"
                    return summary

                result_path = require_repo_path(suite["run_root"]) / sanitize(suite["run_id"]) / "suite_result.json"
                targets.extend(load_json(result_path)["profile_manifests"])
                write_json(output / "normal_inputs.json", {"target_profile_manifests": targets})
        elif evaluation.get("normal_suites"):
            targets = load_json(output / "normal_inputs.json")["target_profile_manifests"]

        score_output = output / "evaluation"
        command = [
            str(ROOT_DIR / "scripts/model.sh"),
            "evaluate",
            "--output-dir",
            str(repo_relative_path(score_output)),
        ]
        for directory in prediction_dirs:
            command.extend(("--prediction-dir", str(repo_relative_path(directory))))
        for field, option in (
            ("profile_run_dirs", "--profile-run-dir"),
            ("structure_profile_manifests", "--structure-profile-manifest"),
        ):
            for item in evaluation.get(field, []):
                command.extend((option, item))
        for target in targets:
            command.extend(("--target-profile-manifest", target))
        for field, option in (("normal_http", "--normal-http"), ("oracle_cost_replay", "--oracle-cost-replay")):
            if evaluation.get(field, False):
                command.append(option)
        if "oracle_max_runs" in evaluation:
            command.extend(("--oracle-max-runs", str(evaluation["oracle_max_runs"])))
        for field in ("model_run_jobs", "trace_threads", "trace_file_threads", "diagnostics"):
            if field in execution:
                command.extend(("--" + field.replace("_", "-"), str(execution[field])))

        summary["status"] = "evaluating"
        write_json(report, summary)
        gate = score_output / "gate_summary.json"
        gate.unlink(missing_ok=True)
        code = run_command(command).returncode
        if not gate.is_file():
            summary.update(status="evaluation_failed", evaluation_return_code=code)
            return summary

        score = load_json(gate)
        summary.update(status=score["status"], evaluation=str(repo_relative_path(gate)), evaluation_return_code=code)
        return summary
    except KeyboardInterrupt:
        summary["status"] = "interrupted"
        raise
    except BaseException as error:
        summary.update(status="failed", error=f"{type(error).__name__}: {error}")
        raise
    finally:
        summary["wall_seconds"] = time.monotonic() - started
        write_json(report, summary)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--request", required=True, type=Path)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--predict-only", action="store_true")
    mode.add_argument("--score-only", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)
    result = run_experiment(
        require_repo_path(args.request),
        dry_run=args.dry_run,
        predict_only=args.predict_only,
        score_only=args.score_only,
    )
    print(f"experiment={result['status']} groups={result['group_count']}", flush=True)
    return 0 if result["status"] in {"planned", "predicted", "PASS"} else 2


if __name__ == "__main__":
    raise SystemExit(main())
