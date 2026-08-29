"""Strict-serial HTTP execution, read-only HiCache gates, and forced-token replay."""

from __future__ import annotations

import json
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any, Iterable, Mapping

from ..contracts.forced_token.plan import (
    index_plan_requests_by_logical_id,
    int_list,
    load_forced_token_plan,
    validate_plan_contract,
)
from .diagnostics import assertion_matches, evaluate_assertion
from .expand import CanonicalPlan, RequestPlan
from .report import latency_summary, write_outputs
from .schema import ConfigSpec, TemplateValidationError


NO_PROXY_OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


class WorkloadExecutionError(RuntimeError):
    """Raised after a report was written for a failed immutable workload sequence."""


def execute_workload(
    plan: CanonicalPlan,
    *,
    base_url: str,
    output_dir: Path,
    mode: str,
    forced_token_plan_path: Path | None,
    config: ConfigSpec | None,
    timeout_sec: float,
    diagnostic_api_key: str | None,
    require_diagnostic: bool,
) -> bool:
    """Execute one fixed plan; any gate error stops instead of changing request order."""

    if mode not in {"none", "capture", "replay"}:
        raise TemplateValidationError(f"unsupported forced-token mode: {mode}")
    if mode == "replay" and forced_token_plan_path is None:
        raise TemplateValidationError("replay requires a forced token plan")
    if mode == "capture" and forced_token_plan_path is None:
        forced_token_plan_path = output_dir / "forced_token_plan.json"
    if mode == "capture" and forced_token_plan_path is not None and forced_token_plan_path.exists():
        raise TemplateValidationError(f"capture plan is immutable and already exists: {forced_token_plan_path}")
    if require_diagnostic and config is None:
        raise TemplateValidationError("no-profile HiCache replay requires a resolved config contract")

    output_dir.mkdir(parents=True, exist_ok=True)
    replay_requests: dict[str, Mapping[str, Any]] = {}
    if mode == "replay":
        assert forced_token_plan_path is not None
        replay_requests = _validate_replay_plan(plan, forced_token_plan_path)
    rows: list[dict[str, Any]] = []
    captured_requests: list[dict[str, Any]] = []
    formal_begin_ms: float | None = None
    formal_end_ms: float | None = None
    stopped_reason: str | None = None
    diagnostic_url = base_url.rstrip("/") + "/hicache/diagnostic"
    if require_diagnostic:
        startup_error = _validate_startup_snapshot(
            _diagnostic_get(diagnostic_url, timeout_sec, diagnostic_api_key), config
        )
        startup_row = {
            "kind": "startup_gate",
            "step_id": "startup_gate",
            "sequence_id": -1,
            "phase": "startup",
            "measure": False,
            "status": "ok" if startup_error is None else "error",
        }
        if startup_error is not None:
            startup_row["failure_reason"] = startup_error
            stopped_reason = startup_error
        rows.append(startup_row)

    if stopped_reason is None:
        for step in plan.steps:
            if isinstance(step, RequestPlan):
                row, captured = _execute_request(
                    step,
                    base_url=base_url,
                    timeout_sec=timeout_sec,
                    mode=mode,
                    replay_request=replay_requests.get(step.logical_request_id),
                )
                rows.append(row)
                if captured is not None:
                    captured_requests.append(captured)
                if step.step_id == plan.formal_start_step and row.get("status") == "ok":
                    formal_begin_ms = float(row["start_time_ms"])
                if step.step_id == plan.formal_end_step and row.get("status") == "ok":
                    formal_end_ms = float(row["end_time_ms"])
                if row.get("status") != "ok":
                    stopped_reason = str(row.get("failure_reason") or row.get("error") or "request_failed")
                    break
                continue

            if mode == "capture" and step.kind in {"barrier", "checkpoint"}:
                rows.append(
                    {
                        "kind": step.kind,
                        "step_id": step.step_id,
                        "sequence_id": step.sequence_id,
                        "phase": step.phase,
                        "measure": step.measure,
                        "status": "skipped",
                        "reason": "capture_has_no_hicache_state",
                    }
                )
                continue
            if step.kind == "wait":
                row = _execute_explicit_wait(step)
            elif step.kind == "barrier":
                if not require_diagnostic:
                    row = _gate_not_available_row(step, "diagnostic_required")
                else:
                    row = _execute_barrier(
                        step,
                        diagnostic_url=diagnostic_url,
                        timeout_sec=timeout_sec,
                        api_key=diagnostic_api_key,
                    )
            elif step.kind == "checkpoint":
                if not require_diagnostic or config is None:
                    row = _gate_not_available_row(step, "diagnostic_required")
                else:
                    row = _execute_checkpoint(
                        step,
                        plan=plan,
                        config=config,
                        diagnostic_url=diagnostic_url,
                        timeout_sec=timeout_sec,
                        api_key=diagnostic_api_key,
                    )
            else:
                row = _gate_not_available_row(step, f"unsupported_step_kind:{step.kind}")
            rows.append(row)
            if row.get("status") != "ok":
                stopped_reason = str(row.get("failure_reason") or "gate_failed")
                break

    forced_summary = _forced_token_summary(
        mode=mode,
        plan=plan,
        forced_token_plan_path=forced_token_plan_path,
        rows=rows,
        capture_written=False,
    )
    if (
        mode == "capture"
        and stopped_reason is None
        and len(captured_requests) == len(plan.requests)
        and forced_token_plan_path is not None
    ):
        captured_plan = _build_captured_forced_plan(plan, captured_requests)
        forced_token_plan_path.parent.mkdir(parents=True, exist_ok=True)
        forced_token_plan_path.write_text(
            json.dumps(captured_plan, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        forced_summary = _forced_token_summary(
            mode=mode,
            plan=plan,
            forced_token_plan_path=forced_token_plan_path,
            rows=rows,
            capture_written=True,
        )

    if formal_begin_ms is None or formal_end_ms is None:
        formal_window = {
            "status": "incomplete",
            "formal_begin_ms": formal_begin_ms,
            "formal_end_ms": formal_end_ms,
            "e2e_ms": None,
        }
        if stopped_reason is None:
            stopped_reason = "formal_window_incomplete"
    else:
        formal_window = {
            "status": "ok",
            "formal_begin_ms": formal_begin_ms,
            "formal_end_ms": formal_end_ms,
            "e2e_ms": formal_end_ms - formal_begin_ms,
        }
    success = stopped_reason is None and _forced_summary_ready(forced_summary)
    summary = {
        "status": "completed" if success else "failed",
        "failure_reason": stopped_reason,
        "workload_id": plan.template.workload_id,
        "config_id": config.config_id if config is not None else None,
        "mode": mode,
        "forced_token": forced_summary,
        "formal_window": formal_window,
        "total": latency_summary(rows),
        "measure_true": latency_summary([row for row in rows if row.get("measure") is True]),
        "requests": rows,
    }
    write_outputs(output_dir, summary=summary)
    if not success:
        raise WorkloadExecutionError(stopped_reason or "forced_token_contract_failed")
    return True


def _validate_replay_plan(plan: CanonicalPlan, plan_path: Path) -> dict[str, Mapping[str, Any]]:
    """Refuse replay before the first request when any immutable identity differs."""

    forced_plan = load_forced_token_plan(plan_path)
    expected_ids = [request.logical_request_id for request in plan.requests]
    errors = validate_plan_contract(
        forced_plan,
        workload_id=plan.template.workload_id,
        expected_request_ids=expected_ids,
    )
    if errors:
        raise TemplateValidationError("forced token replay plan rejected: " + ", ".join(errors))
    indexed = index_plan_requests_by_logical_id(forced_plan)
    for request in plan.requests:
        captured = indexed[request.logical_request_id]
        captured_input_ids = int_list(captured.get("origin_input_ids"))
        if captured_input_ids is None or tuple(captured_input_ids) != request.prompt_token_ids:
            raise TemplateValidationError(f"forced plan origin input mismatch: {request.logical_request_id}")
    return indexed


def _execute_request(
    request: RequestPlan,
    *,
    base_url: str,
    timeout_sec: float,
    mode: str,
    replay_request: Mapping[str, Any] | None,
) -> tuple[dict[str, Any], dict[str, Any] | None]:
    """Send one synchronous HTTP request and record only observed response facts."""

    if mode == "replay":
        if replay_request is None:
            raise TemplateValidationError(f"missing replay request for {request.logical_request_id}")
        body = _replay_body(replay_request)
    else:
        body = _normal_body(request, capture=mode == "capture")
    response = _post_json(base_url.rstrip("/") + "/generate", body, timeout_sec, None)
    row: dict[str, Any] = {
        "kind": "request",
        "step_id": request.step_id,
        "sequence_id": request.sequence_id,
        "logical_request_id": request.logical_request_id,
        "request_name": request.request_name,
        "phase": request.phase,
        "measure": request.measure,
        "origin_input_count": len(request.prompt_token_ids),
        "anchor_tokens": request.anchor_tokens,
        "tail_tokens": request.tail_tokens,
        **response,
    }
    captured: dict[str, Any] | None = None
    if mode == "capture" and row.get("status") == "ok":
        captured = _capture_request_record(request, row)
        if captured is None:
            row["status"] = "error"
            row["failure_reason"] = "capture_response_missing_token_ids"
    if mode == "replay":
        _apply_replay_checks(row, replay_request)
    row.pop("_response_json", None)
    if row.get("status") != "ok" and "failure_reason" not in row:
        row["failure_reason"] = str(row.get("error") or "request_failed")
    return row, captured


def _normal_body(request: RequestPlan, *, capture: bool) -> dict[str, Any]:
    """Build deterministic capture/normal body without config-dependent branches."""

    body = {
        "text": request.prompt,
        "sampling_params": {
            "max_new_tokens": 1,
            "temperature": 0,
            "top_p": 1.0,
            "top_k": 1,
            "ignore_eos": True,
            "sampling_seed": 20260729,
        },
    }
    if capture:
        body["return_prompt_token_ids"] = True
    return body


def _replay_body(captured_request: Mapping[str, Any]) -> dict[str, Any]:
    """Build forced-output replay body from captured origin/output token ids."""

    origin_input_ids = int_list(captured_request.get("origin_input_ids"))
    forced_output_ids = int_list(captured_request.get("forced_output_ids"))
    if origin_input_ids is None or forced_output_ids is None:
        raise TemplateValidationError("forced replay request is missing token arrays")
    return {
        "input_ids": origin_input_ids,
        "rid": captured_request["logical_request_id"],
        "return_prompt_token_ids": True,
        "sampling_params": {
            "max_new_tokens": len(forced_output_ids),
            "temperature": 0,
            "top_p": 1.0,
            "top_k": 1,
            "ignore_eos": True,
            "sampling_seed": 20260729,
        },
        "trace_sim_forced_output_ids": forced_output_ids,
    }


def _post_json(
    url: str,
    body: Mapping[str, Any],
    timeout_sec: float,
    api_key: str | None,
) -> dict[str, Any]:
    """Perform a synchronous JSON POST with wall and monotonic latency facts."""

    headers = {"Content-Type": "application/json"}
    if api_key:
        headers["Authorization"] = f"Bearer {api_key}"
    request = urllib.request.Request(
        url,
        data=json.dumps(dict(body)).encode("utf-8"),
        headers=headers,
        method="POST",
    )
    start_monotonic = time.perf_counter() * 1000.0
    start_wall = time.time() * 1000.0
    try:
        with NO_PROXY_OPENER.open(request, timeout=timeout_sec) as response:
            payload = response.read()
            http_status = response.status
        end_monotonic = time.perf_counter() * 1000.0
        return {
            "status": "ok",
            "http_status": http_status,
            "response_bytes": len(payload),
            "start_time_ms": start_wall,
            "end_time_ms": time.time() * 1000.0,
            "latency_ms": end_monotonic - start_monotonic,
            **_response_facts(payload),
        }
    except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError, OSError) as error:
        end_monotonic = time.perf_counter() * 1000.0
        return {
            "status": "error",
            "error": type(error).__name__,
            "error_message": str(error),
            "start_time_ms": start_wall,
            "end_time_ms": time.time() * 1000.0,
            "latency_ms": end_monotonic - start_monotonic,
            "response_bytes": 0,
        }


def _response_facts(payload: bytes) -> dict[str, Any]:
    """Extract response token facts needed by capture/replay contracts."""

    try:
        parsed = json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        return {}
    if not isinstance(parsed, dict):
        return {}
    result: dict[str, Any] = {}
    result["_response_json"] = parsed
    output_ids = int_list(parsed.get("output_ids"))
    prompt_ids = int_list(parsed.get("prompt_token_ids"))
    if output_ids is not None:
        result["actual_output_count"] = len(output_ids)
    if prompt_ids is not None:
        result["actual_prompt_token_count"] = len(prompt_ids)
    return result


def _capture_request_record(request: RequestPlan, row: Mapping[str, Any]) -> dict[str, Any] | None:
    """Construct one replay request from observed server tokens."""

    response = row.get("_response_json")
    if not isinstance(response, dict):
        return None
    origin_input_ids = int_list(response.get("prompt_token_ids"))
    output_ids = int_list(response.get("output_ids"))
    if origin_input_ids is None or output_ids is None:
        return None
    if tuple(origin_input_ids) != request.prompt_token_ids:
        return None
    return {
        "logical_request_id": request.logical_request_id,
        "origin_input_ids": origin_input_ids,
        "forced_output_ids": output_ids,
    }


def _apply_replay_checks(row: dict[str, Any], captured_request: Mapping[str, Any] | None) -> None:
    """Record exact forced-output and origin-input checks for a replay request."""

    if captured_request is None:
        row["status"] = "error"
        row["failure_reason"] = "forced_token_plan_missing_request"
        return
    response = row.pop("_response_json", None)
    expected_output_ids = int_list(captured_request.get("forced_output_ids")) or []
    expected_input_ids = int_list(captured_request.get("origin_input_ids")) or []
    actual_output_ids = int_list(response.get("output_ids")) if isinstance(response, dict) else None
    actual_input_ids = int_list(response.get("prompt_token_ids")) if isinstance(response, dict) else None
    row.update(
        {
            "forced_output_count": len(expected_output_ids),
            "forced_token_output_checked": actual_output_ids is not None,
            "actual_output_matches_forced": actual_output_ids == expected_output_ids,
            "actual_prompt_matches_plan": actual_input_ids == expected_input_ids,
        }
    )
    if row.get("status") == "ok" and (
        actual_output_ids is None
        or actual_input_ids is None
        or actual_output_ids != expected_output_ids
        or actual_input_ids != expected_input_ids
    ):
        row["status"] = "error"
        row["failure_reason"] = "forced_token_replay_mismatch"


def _execute_explicit_wait(step: Any) -> dict[str, Any]:
    """Execute the only allowed explicit wait, preserving it in the report."""

    duration_ms = int(step.details["duration_ms"])
    start_wall = time.time() * 1000.0
    start_monotonic = time.perf_counter() * 1000.0
    time.sleep(duration_ms / 1000.0)
    return {
        "kind": "wait",
        "step_id": step.step_id,
        "sequence_id": step.sequence_id,
        "phase": step.phase,
        "measure": step.measure,
        "status": "ok",
        "duration_ms": duration_ms,
        "start_time_ms": start_wall,
        "end_time_ms": time.time() * 1000.0,
        "latency_ms": time.perf_counter() * 1000.0 - start_monotonic,
    }


def _execute_barrier(
    step: Any,
    *,
    diagnostic_url: str,
    timeout_sec: float,
    api_key: str | None,
) -> dict[str, Any]:
    """Poll a read-only idle snapshot until queues drain or the fixed gate times out."""

    deadline = time.monotonic() + float(step.details["timeout_sec"])
    start_wall = time.time() * 1000.0
    polls = 0
    while time.monotonic() <= deadline:
        polls += 1
        try:
            snapshot = _diagnostic_get(diagnostic_url, timeout_sec, api_key)
        except WorkloadExecutionError as error:
            return _gate_failure_row(step, "diagnostic_unavailable", start_wall, polls, str(error))
        if _diagnostic_idle(snapshot):
            return {
                "kind": "barrier",
                "step_id": step.step_id,
                "sequence_id": step.sequence_id,
                "phase": step.phase,
                "measure": step.measure,
                "scope": step.details["scope"],
                "status": "ok",
                "start_time_ms": start_wall,
                "end_time_ms": time.time() * 1000.0,
                "wait_ms": time.time() * 1000.0 - start_wall,
                "polls": polls,
            }
        time.sleep(0.1)
    return _gate_failure_row(step, "barrier_timeout", start_wall, polls, "hicache_idle_not_observed")


def _execute_checkpoint(
    step: Any,
    *,
    plan: CanonicalPlan,
    config: ConfigSpec,
    diagnostic_url: str,
    timeout_sec: float,
    api_key: str | None,
) -> dict[str, Any]:
    """Evaluate fixed witness assertions against one read-only diagnostic snapshot."""

    start_wall = time.time() * 1000.0
    try:
        snapshot = _diagnostic_get(diagnostic_url, timeout_sec, api_key)
    except WorkloadExecutionError as error:
        return _gate_failure_row(step, "diagnostic_unavailable", start_wall, 1, str(error))
    assertion_rows: list[dict[str, Any]] = []
    all_passed = True
    request_by_name = plan.request_by_name
    for assertion in step.details["assertions"]:
        request_name = str(assertion["request"])
        request = request_by_name.get(request_name)
        if request is None:
            assertion_rows.append({"id": assertion["id"], "passed": False, "failure_reason": "unknown_request"})
            all_passed = False
            continue
        observed = evaluate_assertion(snapshot, request, str(assertion["range"]), config)
        expected = assertion["expect"]
        passed = assertion_matches(observed, expected)
        assertion_rows.append(
            {
                "id": assertion["id"],
                "request": request_name,
                "range": assertion["range"],
                "expected": expected,
                "observed": observed,
                "passed": passed,
            }
        )
        all_passed &= passed
    return {
        "kind": "checkpoint",
        "step_id": step.step_id,
        "sequence_id": step.sequence_id,
        "phase": step.phase,
        "measure": step.measure,
        "status": "ok" if all_passed else "error",
        "failure_reason": None if all_passed else "checkpoint_assertion_failed",
        "start_time_ms": start_wall,
        "end_time_ms": time.time() * 1000.0,
        "assertions": assertion_rows,
    }


def _diagnostic_get(url: str, timeout_sec: float, api_key: str | None) -> dict[str, Any]:
    """Fetch the admin-only, read-only HiCache diagnostic snapshot."""

    headers: dict[str, str] = {}
    if api_key:
        headers["Authorization"] = f"Bearer {api_key}"
    request = urllib.request.Request(url, headers=headers, method="GET")
    try:
        with NO_PROXY_OPENER.open(request, timeout=timeout_sec) as response:
            payload = response.read()
            if response.status != 200:
                raise WorkloadExecutionError(f"diagnostic_http_status:{response.status}")
    except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError, OSError) as error:
        raise WorkloadExecutionError(f"diagnostic_request_failed:{type(error).__name__}:{error}") from error
    try:
        parsed = json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise WorkloadExecutionError("diagnostic_invalid_json") from error
    if not isinstance(parsed, dict) or parsed.get("schema") != "sglang.hicache.diagnostic":
        raise WorkloadExecutionError("diagnostic_schema_mismatch")
    return parsed


def _diagnostic_idle(snapshot: Mapping[str, Any]) -> bool:
    """Require all scheduler ranks to report queues and background operations drained."""

    ranks = snapshot.get("ranks")
    return (
        isinstance(ranks, list)
        and bool(ranks)
        and all(isinstance(rank, dict) and rank.get("hicache_idle") is True for rank in ranks)
    )


def _validate_startup_snapshot(snapshot: Mapping[str, Any], config: ConfigSpec | None) -> str | None:
    """Check actual resolved cache/scheduler state before entering a cell sequence."""

    if config is None:
        return "missing_config_contract"
    ranks = snapshot.get("ranks")
    if not isinstance(ranks, list) or not ranks:
        return "startup_diagnostic_has_no_ranks"
    for rank in ranks:
        if not isinstance(rank, dict):
            return "startup_diagnostic_invalid_rank"
        if rank.get("cache_class") != "HiRadixCache":
            return f"cache_class_mismatch:{rank.get('cache_class')}"
        resolved = rank.get("resolved")
        if not isinstance(resolved, dict):
            return "startup_resolved_state_missing"
        expected = {
            "page_size": config.page_size,
            "device_pages": config.device_pages,
            "host_pages": config.host_pages,
            "write_policy": config.write_policy,
            "prefetch_threshold": config.prefetch_threshold,
            "prefetch_stop_policy": config.prefetch_stop_policy,
            "prefetch_capacity_limit_tokens": config.prefetch_capacity_limit_tokens,
        }
        for field, value in expected.items():
            if resolved.get(field) != value:
                return f"startup_{field}_mismatch:expected={value}:actual={resolved.get(field)}"
        if config.prefetch_stop_policy == "timeout":
            timeout = resolved.get("prefetch_timeout")
            expected_timeout = {
                "base_sec": config.prefetch_timeout_base_sec,
                "per_ki_token_sec": config.prefetch_timeout_per_ki_token_sec,
                "max_sec": config.prefetch_timeout_max_sec,
            }
            if timeout != expected_timeout:
                return f"startup_timeout_mismatch:expected={expected_timeout}:actual={timeout}"
    return None


def _gate_not_available_row(step: Any, reason: str) -> dict[str, Any]:
    """Construct a terminal row when a required read-only gate is unavailable."""

    return {
        "kind": step.kind,
        "step_id": step.step_id,
        "sequence_id": step.sequence_id,
        "phase": step.phase,
        "measure": step.measure,
        "status": "error",
        "failure_reason": reason,
    }


def _gate_failure_row(
    step: Any,
    reason: str,
    start_wall: float,
    polls: int,
    detail: str,
) -> dict[str, Any]:
    """Construct an auditable terminal barrier/checkpoint error row."""

    return {
        "kind": step.kind,
        "step_id": step.step_id,
        "sequence_id": step.sequence_id,
        "phase": step.phase,
        "measure": step.measure,
        "status": "error",
        "failure_reason": reason,
        "error_message": detail,
        "start_time_ms": start_wall,
        "end_time_ms": time.time() * 1000.0,
        "polls": polls,
    }


def _build_captured_forced_plan(plan: CanonicalPlan, requests: list[dict[str, Any]]) -> dict[str, Any]:
    """Build the immutable standard forced-token plan after a complete capture."""

    return {
        "workload_id": plan.template.workload_id,
        "requests": requests,
    }


def _forced_token_summary(
    *,
    mode: str,
    plan: CanonicalPlan,
    forced_token_plan_path: Path | None,
    rows: Iterable[Mapping[str, Any]],
    capture_written: bool,
) -> dict[str, Any]:
    """Create the stable forced-token quality object consumed by suite aggregation."""

    request_rows = [row for row in rows if row.get("kind") == "request"]
    plan_exists = bool(forced_token_plan_path and forced_token_plan_path.is_file())
    replay_rows = request_rows if mode == "replay" else []
    unchecked = [row for row in replay_rows if not row.get("forced_token_output_checked")]
    output_mismatches = [row for row in replay_rows if row.get("actual_output_matches_forced") is False]
    prompt_mismatches = [row for row in replay_rows if row.get("actual_prompt_matches_plan") is False]
    replay_ready = (
        bool(replay_rows)
        and not unchecked
        and not output_mismatches
        and not prompt_mismatches
        and plan_exists
    )
    return {
        "enabled": mode != "none",
        "mode": mode,
        "ready": capture_written if mode == "capture" else replay_ready if mode == "replay" else True,
        "workload_mode": "normal" if mode == "none" else f"forced_token_{mode}",
        "workload_id": plan.template.workload_id,
        "plan_path": str(forced_token_plan_path) if forced_token_plan_path else None,
        "plan_workload_id": plan.template.workload_id if plan_exists else None,
        "request_count": len(request_rows),
        "output_checked_count": len(replay_rows) - len(unchecked),
        "mismatch_count": len(output_mismatches),
        "unchecked_count": len(unchecked),
        "prompt_mismatch_count": len(prompt_mismatches),
        "all_actual_outputs_match_plan": (not output_mismatches and not unchecked and not prompt_mismatches)
        if mode == "replay"
        else None,
        "plan_written": capture_written if mode == "capture" else None,
    }


def _forced_summary_ready(summary: Mapping[str, Any]) -> bool:
    """Return only the explicit forced-token readiness bit."""

    return bool(summary.get("ready"))
