"""Run the declared search demonstration through existing prediction/capture APIs.

This is an external example, not another predictor or performance database.
The task directory's protocol and input requests must be prepared before use.
"""

from __future__ import annotations

import argparse
from dataclasses import asdict
from pathlib import Path
import time
from typing import Any

from markov_internal.common.io import load_json, write_json
from markov_internal.common.paths import repo_relative_path, require_repo_path
from markov_internal.common.process import run_command
from markov_internal.contracts.forced_token.bundle import resolve_forced_token_bundle_plan
from markov_internal.modeling.sglang_config import hicache_workload_spec
from markov_internal.modeling.source_dag.profiles import discover_profile_runs
from markov_internal.modeling.types import TargetHiCacheConfig
from markov_internal.preparation.capture import ProfileBudget
from markov_internal.preparation.profile_capture import ProfileCaptureJob, capture_profile_jobs, normal_confirmation_job
from markov_internal.preparation.resources import wait_for_normal_resources


def public_command(root: Path, label: str, command: list[str], accepted: tuple[int, ...] = (0,)) -> int:
    """Keep the actual invocation log, including successful current executions."""

    path = root / "invocations" / f"{label}_{time.time_ns()}.log"
    print(f"{label}: {path}", flush=True)
    result = run_command(command, log_path=path)
    if result.returncode not in accepted:
        raise RuntimeError(f"{label}: exit {result.returncode}; see {path}")

    return result.returncode


def search(root: Path, *, bounded: bool) -> None:
    protocol = load_json(root / "protocol.json")
    requests = (
        [root / "inputs/budget_search.json"]
        if bounded
        else [root / "inputs" / f"search_{source['workload']}.json" for source in protocol["sources"]]
    )
    for request in requests:
        declaration = load_json(request)
        output = require_repo_path(declaration["output_dir"])
        summary_path = output / "search_summary.json"
        if summary_path.exists():
            previous = load_json(summary_path)
            if previous["status"] in {"completed", "completed_with_gaps"}:
                print(f"Already complete within this unchanged comparison: {output}", flush=True)
                continue

        command = ["scripts/model.sh", "search", "--request", str(repo_relative_path(request))]
        if summary_path.exists():
            command.append("--resume")
        public_command(root, request.stem, command)


def select(root: Path) -> dict[str, Any]:
    """Choose candidates without reading a normal capture or evaluator output."""

    path = root / "selection.json"
    if path.exists():
        return load_json(path)

    protocol = load_json(root / "protocol.json")
    selection = {"created_at_unix": time.time(), "workloads": {}, "rule": protocol["normal_confirmation"]}
    for source in protocol["sources"]:
        workload = source["workload"]
        state = load_json(root / "search" / workload / "search_summary.json")
        if state["status"] not in {"completed", "completed_with_gaps"}:
            raise ValueError(f"full search is still incomplete: {workload}")

        ordered = sorted(
            (row for row in state["candidates"] if row["score_sec"] is not None),
            key=lambda row: (row["score_sec"], row["space_index"]),
        )
        chosen = {row["space_index"] for row in ordered[:2]}
        if workload == "sharegpt_revisit":
            chosen.update(protocol["normal_confirmation"]["sharegpt_reference_indices"])
        else:
            chosen.update(row["space_index"] for row in ordered if row["name"] == protocol["base"])
            competitor = next((row for row in ordered if row["score_sec"] > ordered[1]["score_sec"]), None)
            if competitor is not None:
                chosen.add(competitor["space_index"])

        selected = [row for row in state["candidates"] if row["space_index"] in chosen]
        if any(row["score_sec"] is None for row in selected):
            raise ValueError(f"predeclared normal comparison still has missing predictions: {workload}")

        selection["workloads"][workload] = {
            "source": source,
            "targets": [
                row
                for row in sorted(state["candidates"], key=lambda row: row["space_index"])
                if row["space_index"] in chosen
            ],
            "all_predicted_best": state["best_candidates"],
            "predicted_best_sec": state["best_score_sec"],
            "global_model_optimum_proven": state["global_model_optimum_proven"],
            "unpredicted_candidates": [row["name"] for row in state["candidates"] if row["score_sec"] is None],
            "prediction_dirs": [attempt["prediction_dir"] for attempt in state["attempts"] if attempt["report"]],
        }

    write_json(path, selection)
    return selection


def normal_jobs(root: Path, selection: dict[str, Any]) -> list[ProfileCaptureJob]:
    configs = {
        row["name"]: hicache_workload_spec(row["name"], row["hicache"])
        for group in selection["workloads"].values()
        for row in group["targets"]
    }
    specs = root / "inputs/normal_configs.json"
    write_json(specs, {"configs": list(configs.values())})
    sources = {
        source.input_id: source
        for source in discover_profile_runs(
            tuple(require_repo_path(group["source"]["manifest"]) for group in selection["workloads"].values())
        )
    }

    jobs = []
    protocol = load_json(root / "protocol.json")
    repetitions = protocol["normal_confirmation"]["repetitions"]
    for repetition in range(repetitions):
        for workload, group in selection["workloads"].items():
            targets = group["targets"]
            offset = repetition % len(targets)
            plan = resolve_forced_token_bundle_plan(require_repo_path(protocol["forced_token_bundle"]), workload)
            for target in targets[offset:] + targets[:offset]:
                config = TargetHiCacheConfig.from_raw({"name": target["name"], "hicache": target["hicache"]})
                jobs.append(
                    normal_confirmation_job(
                        sources[workload], config, root, specs, plan, repetition=repetition + 1, index=len(jobs) + 1
                    )
                )
    return jobs


def normal(root: Path, *, dry_run: bool, keep_failures: bool = False) -> None:
    selection = select(root)
    jobs = normal_jobs(root, selection)
    declared = load_json(root / "protocol.json")["budget"]
    retry_starts = declared["normal_server_starts"] - len(jobs)
    budget = ProfileBudget(
        wall_seconds=declared["normal_wall_seconds"],
        server_starts=declared["normal_server_starts"],
        requests=sum(job.counts["requests"] for job in jobs)
        + retry_starts * max(job.counts["requests"] for job in jobs),
        tokens=sum(job.counts["tokens"] for job in jobs) + retry_starts * max(job.counts["tokens"] for job in jobs),
    )
    output = root / "normal"
    write_json(
        output / "plan.json",
        {"budget": asdict(budget), "scheduled_successes": len(jobs), "scope": "normal score-only; no training"},
    )
    if dry_run:
        result = capture_profile_jobs(jobs, output, budget, dry_run=True, purpose="confirmation")
        write_json(output / "dry_run.json", result)
        print(f"Normal plan: {len(jobs)} runs, budget={asdict(budget)}", flush=True)
        return

    pending = jobs
    ledger = output / "confirmation_capture_ledger.json"
    history = load_json(ledger)["attempts"] if ledger.exists() else []
    retained_failures = [row for row in history if row["status"] == "failed"] if keep_failures else []
    while pending:
        failed = []
        for job in pending:
            if keep_failures and any(row["inputs"] == job.inputs for row in retained_failures):
                continue

            reuse = capture_profile_jobs([job], output, budget, dry_run=True, purpose="confirmation")
            if reuse["status"] == "profiles_captured":
                continue

            wait_for_normal_resources(output)
            result = capture_profile_jobs([job], output, budget, dry_run=False, purpose="confirmation")
            write_json(output / "latest_capture.json", result)
            if result["status"] in {"capture_budget_exhausted", "capture_incomplete", "cleanup_incomplete"}:
                raise RuntimeError(f"Normal capture stopped: {result['status']}; history retained in {output}")
            if result["status"] != "profiles_captured":
                if keep_failures and result["status"] == "failed":
                    retained_failures.append(result["attempt"])
                    continue

                failed.append(job)
        pending = failed

    if keep_failures:
        write_json(
            output / "pass_summary.json",
            {
                "status": "completed_with_failures" if retained_failures else "completed",
                "scheduled_jobs": len(jobs),
                "failed_attempts": len(retained_failures),
                "scope": "Each declared repetition attempted once. Failed attempts are retained, never retried on resume.",
            },
        )
        print(f"Normal capture pass complete; {len(retained_failures)} failures retained; ledger={output}", flush=True)
        return

    print(f"Normal captures complete: {len(jobs)} successful jobs; ledger={output}", flush=True)


def evaluate(root: Path) -> None:
    selection = load_json(root / "selection.json")
    ledger = load_json(root / "normal/confirmation_capture_ledger.json")
    manifests = [row["profile_manifest"] for row in ledger["attempts"] if row["status"] == "completed"]
    expected = selection["rule"]["repetitions"] * sum(
        len(group["targets"]) for group in selection["workloads"].values()
    )
    if len(manifests) != expected:
        raise ValueError("normal repetitions are not complete; do not score a smaller subset")

    for workload, group in selection["workloads"].items():
        command = [
            "scripts/model.sh",
            "evaluate",
            "--normal-http",
            "--output-dir",
            str(repo_relative_path(root / "evaluation" / workload)),
        ]
        for directory in group["prediction_dirs"]:
            command.extend(("--prediction-dir", directory))
        for target in group["targets"]:
            command.extend(("--target-config", target["name"]))
        for manifest in manifests:
            command.extend(("--target-profile-manifest", manifest))
        public_command(root, f"evaluate_{workload}", command, accepted=(0, 2))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phase", choices=("search", "budget-search", "select", "normal", "evaluate"))
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--dry-run", action="store_true", help="Prepare only the normal capture plan.")
    parser.add_argument("--keep-failures", action="store_true", help="Attempt each declared normal repetition once.")
    args = parser.parse_args()
    if args.dry_run and args.phase != "normal":
        parser.error("--dry-run is only supported for the normal capture plan")
    if args.keep_failures and args.phase != "normal":
        parser.error("--keep-failures is only supported for normal capture")

    root = args.root.resolve()

    if args.phase in {"search", "budget-search"}:
        search(root, bounded=args.phase == "budget-search")
    elif args.phase == "select":
        result = select(root)
        print({name: len(group["targets"]) for name, group in result["workloads"].items()})
    elif args.phase == "normal":
        normal(root, dry_run=args.dry_run, keep_failures=args.keep_failures)
    else:
        evaluate(root)


if __name__ == "__main__":
    main()
