"""Diagnostic replay of target-observed costs on predicted HiCache DAGs."""

from __future__ import annotations

import subprocess
from collections import Counter
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from tempfile import TemporaryDirectory
from typing import Any

from .....common.io import load_json, write_json
from .....common.paths import ROOT_DIR, resolve_repo_path
from .....modeling.cpp_config import trace_graph_executable
from .matching import build_pair_oracle_override, build_score_only_target_oracle_catalog


def run_suite(
    modeling_run_dir: Path,
    *,
    base_observations_path: Path,
    target_oracle_bundle_path: Path,
    source_config_id: str,
    jobs: int = 4,
    max_runs: int | None = None,
) -> dict[str, Any]:
    """Replay selected cells and retain one compact score summary."""

    ledgers = _ready_ledgers(modeling_run_dir / "artifacts/debug_rows", source_config_id)
    raw_bundle = load_json(target_oracle_bundle_path)
    base_id = _base_id(ledgers, raw_bundle)
    target_cells = load_target_oracle_bundle(target_oracle_bundle_path, base_id)["cells"]
    observed = {(str(cell["workload_id"]), str(cell["run_id"])): cell for cell in target_cells}
    base = _base_by_workload(load_json(base_observations_path))
    if max_runs is None and len(ledgers) != len(target_cells):
        raise ValueError(f"oracle-cost replay has {len(ledgers)} ledgers for {len(target_cells)} score cells")
    if max_runs is not None:
        ledgers = sorted(ledgers, key=lambda row: str(row["model_run_id"]))[:max_runs]

    work = []
    for ledger in sorted(ledgers, key=lambda row: str(row["model_run_id"])):
        key = (str(ledger["workload_id"]), str(ledger["target_run_id"]))
        target = observed.get(key)
        source = base.get(key[0])
        if target is None or source is None:
            raise ValueError(f"missing oracle score input for {key}")
        catalog = build_score_only_target_oracle_catalog(ledger, target)
        work.append((ledger, build_pair_oracle_override(ledger, catalog), _direct_score(ledger, target, source)))
    rows: list[dict[str, Any]] = []
    with ThreadPoolExecutor(max_workers=max(1, jobs)) as pool:
        futures = {
            pool.submit(_run_one, modeling_run_dir, ledger, override, score): ledger for ledger, override, score in work
        }
        for completed, future in enumerate(as_completed(futures), 1):
            row = future.result()
            rows.append(row)
            print(f"oracle-cost {completed}/{len(work)} {row['status']} {row['pair_id']}", flush=True)
    summary = _summary(sorted(rows, key=lambda row: str(row["model_run_id"])), len(work))
    write_json(modeling_run_dir / f"artifacts/oracle_cost_replay/{source_config_id}/summary.json", summary)
    return summary


def _ready_ledgers(directory: Path, source_config_id: str) -> list[dict[str, Any]]:
    rows = [load_json(path) for path in directory.glob("*.json")]
    ready = [
        row for row in rows
        if row.get("status") == "READY" and row.get("model_run_id") and row.get("source_config_id") == source_config_id
    ]
    if not ready or any(row.get("is_self") is True for row in ready):
        raise ValueError("oracle-cost replay requires ready single-base cross ledgers")
    return ready


def _base_id(ledgers: list[dict[str, Any]], bundle: Any) -> str:
    declared = str(bundle.get("base_config_id") or "") if isinstance(bundle, dict) else ""
    sources = {str(row.get("source_config_id") or "") for row in ledgers}
    if not declared or sources != {declared}:
        raise ValueError(f"replay sources {sorted(sources)!r} do not match target oracle base {declared!r}")
    return declared


def load_target_oracle_bundle(path: Path, base_config_id: str) -> dict[str, Any]:
    bundle = load_json(path.resolve())
    cells = bundle.get("cells") if isinstance(bundle, dict) else None
    if (
        str(bundle.get("base_config_id") or "") != base_config_id
        or not isinstance(cells, list)
        or not cells
        or bundle.get("target_profile_used_for_parameters") is not False
        or bundle.get("target_e2e_used") is not False
    ):
        raise ValueError("invalid score-only target oracle bundle")
    keys = {(str(cell.get("config_id") or ""), str(cell.get("workload_id") or "")) for cell in cells}
    if len(keys) != len(cells) or any(not config or not workload or config == base_config_id for config, workload in keys):
        raise ValueError("target oracle cells need unique non-base config/workload identities")
    return bundle


def _totals(cell: dict[str, Any]) -> tuple[int, int]:
    by_kind = cell.get("by_kind") if isinstance(cell.get("by_kind"), dict) else {}
    service = sum(int((row or {}).get("service_us") or 0) for row in by_kind.values())
    control = sum(int((row or {}).get("control_us") or 0) for row in by_kind.values())
    return service, control


def _base_by_workload(payload: Any) -> dict[str, tuple[int, int]]:
    cells = payload.get("cells") if isinstance(payload, dict) else None
    if not isinstance(cells, list) and isinstance(payload, dict) and isinstance(payload.get("by_kind"), dict):
        cells = [payload]
    if not isinstance(cells, list) or not cells:
        raise ValueError("base observations must contain cells")
    result = {str(cell.get("workload_id") or ""): _totals(cell) for cell in cells}
    if len(result) != len(cells) or "" in result:
        raise ValueError("base observation workload identities must be unique")
    return result


def _direct_score(ledger: dict[str, Any], target: dict[str, Any], source: tuple[int, int]) -> dict[str, Any]:
    predicted = _totals(ledger.get("target_predicted") or {})
    actual = _totals(target)
    predicted_us, actual_us, source_us = sum(predicted), sum(actual), sum(source)
    predicted_delta, actual_delta = predicted_us - source_us, actual_us - source_us
    return {
        "predicted_us": predicted_us,
        "actual_us": actual_us,
        "source_us": source_us,
        "predicted_delta_us": predicted_delta,
        "actual_delta_us": actual_delta,
        "large_change": abs(actual_delta) > 50_000,
        "direction_correct": (predicted_delta > 0) == (actual_delta > 0) if actual_delta else predicted_delta == 0,
    }


def _run_one(
    root: Path,
    ledger: dict[str, Any],
    override: dict[str, Any],
    score: dict[str, Any],
) -> dict[str, Any]:
    identity = {
        field: ledger.get(field)
        for field in ("model_run_id", "pair_id", "workload_id", "source_config_id", "target_config_id")
    }
    run_dir = root / "model_runs" / str(identity["model_run_id"])
    runner = load_json(run_dir / "runner_config.json")
    with TemporaryDirectory(prefix="markov_oracle_cost_") as raw_temp:
        temp = Path(raw_temp)
        override_path, summary_path = temp / "costs.json", temp / "run_summary.json"
        write_json(override_path, override)
        completed = subprocess.run(
            _command(runner, run_dir / "cpp_model_config.json", override_path, summary_path),
            cwd=ROOT_DIR,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
        if completed.returncode:
            return {
                **identity,
                "status": "ERROR",
                "errors": ["cpp_oracle_cost_replay_failed"],
                "error_output_tail": completed.stdout[-4096:],
            }
        replay = load_json(summary_path)
    original = load_json(run_dir / "run_summary.json")
    patch = replay.get("module_results", {}).get("hicache_dag_patch", {})
    oracle = patch.get("io_resources", {}).get("oracle_cost_replay", {})
    causal = patch.get("causal_timing_audit", {})
    expected_costs = len(override.get("costs") or [])
    causal_ready = (
        causal.get("status") == "ready" and causal.get("restored_exact") is True
        if expected_costs
        else causal.get("status") == "no_materialized_target_cost_nodes"
    )
    ready = all(
        (
            patch.get("validation", {}).get("status") == "ready",
            patch.get("topology_valid") is True,
            not patch.get("blocker_counts"),
            oracle.get("status") == "ready",
            oracle.get("effect_identity_exact") is True,
            oracle.get("operation_shape_exact") is True,
            oracle.get("target_e2e_consumed") is False,
            causal_ready,
        )
    )
    effects = causal.get("effects") or []
    model_e2e = int(original.get("simulated_e2e_us") or 0)
    oracle_e2e = int(replay.get("simulated_e2e_us") or 0)
    return {
        **identity,
        "status": "READY" if ready else "NOT_READY",
        "errors": [] if ready else ["oracle_structure_or_cost_binding_not_ready"],
        "effect_count": len(effects),
        "cost_response_count": sum(int(row.get("cost_node_completion_response_us") or 0) > 0 for row in effects),
        "direct": score,
        "model_e2e_us": model_e2e,
        "oracle_e2e_us": oracle_e2e,
    }


def _command(runner: dict[str, Any], model: Path, override: Path, summary: Path) -> list[str]:
    cpp = runner["cpp_trace_graph"]
    command = [
        str(trace_graph_executable(runner)),
        "--profile-manifest",
        str(_path(runner["input"]["profile_manifest"])),
        "--run-summary",
        str(summary),
        "--model-config",
        str(model),
        "--hicache-oracle-cost-replay",
        str(override),
    ]
    for name, value in (
        ("--threads", cpp.get("threads")),
        ("--file-threads", cpp.get("file_threads")),
        ("--trace-window-start-us", cpp.get("trace_window_start_us")),
        ("--trace-window-end-us", cpp.get("trace_window_end_us")),
        ("--actual-e2e-us", cpp.get("actual_e2e_us")),
    ):
        if value is not None:
            command.extend((name, str(value)))
    if cpp.get("trace_channels"):
        command.extend(("--trace-channels", ",".join(str(value) for value in cpp["trace_channels"])))
    return command


def _summary(rows: list[dict[str, Any]], expected: int) -> dict[str, Any]:
    ready = [row for row in rows if row.get("status") == "READY"]
    complete = len(rows) == len(ready) == expected
    direct = [row["direct"] for row in ready]
    total = _metrics([(row["predicted_us"], row["actual_us"]) for row in direct], complete)
    delta_error = sum(abs(row["predicted_delta_us"] - row["actual_delta_us"]) for row in direct)
    delta_reference = sum(abs(row["actual_delta_us"]) for row in direct)
    delta_l1 = 100.0 * delta_error / delta_reference if delta_reference else (0.0 if not delta_error else None)
    large = [row for row in direct if row["large_change"]]
    direction = 100.0 * sum(row["direction_correct"] for row in large) / len(large) if large else None
    phase = _metrics(
        [(abs(row["model_e2e_us"] - row["oracle_e2e_us"]), row["direct"]["actual_us"]) for row in ready],
        complete,
        error_is_prediction=True,
    )
    effect_count = sum(int(row.get("effect_count") or 0) for row in ready)
    return {
        "status": "OK" if complete else ("PARTIAL" if ready else "ERROR"),
        "diagnostic_only": True,
        "target_e2e_used": False,
        "prefill_decode_excluded": True,
        "row_count": len(rows),
        "ready_count": len(ready),
        "structure_binding_ready_count": len(ready),
        "causal_effect_count": effect_count,
        "cost_response_count": sum(int(row.get("cost_response_count") or 0) for row in ready),
        "direct_cost": {
            "status": "PASS"
            if total["passed"] and delta_l1 is not None and delta_l1 <= 3.0 and bool(large) and direction == 100.0
            else "FAIL",
            "total": total,
            "delta_weighted_l1_pct": delta_l1,
            "large_change_count": len(large),
            "large_change_direction_accuracy_pct": direction,
        },
        "phase_normalized_e2e": phase,
        "error_counts": dict(sorted(Counter(error for row in rows for error in row.get("errors", [])).items())),
    }


def _metrics(
    pairs: list[tuple[int, int]],
    complete: bool,
    *,
    error_is_prediction: bool = False,
) -> dict[str, Any]:
    errors = [predicted if error_is_prediction else abs(predicted - actual) for predicted, actual in pairs]
    reference = sum(actual for _, actual in pairs)
    percentages = [100.0 * error / actual for error, (_, actual) in zip(errors, pairs) if actual > 0]
    wape = 100.0 * sum(errors) / reference if reference else (0.0 if not sum(errors) else None)
    p90 = sorted(percentages)[round((len(percentages) - 1) * 0.9)] if percentages else None
    passed = complete and wape is not None and p90 is not None and wape <= 3.0 and p90 <= 5.0
    return {"passed": passed, "count": len(pairs), "wape_pct": wape, "p90_pct": p90}


def _path(value: str | Path) -> Path:
    path = resolve_repo_path(value)
    if path is None:
        raise ValueError(f"missing path: {value}")
    return path
