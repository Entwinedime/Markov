"""Human-readable progress output for HiCache validation workflows."""

from __future__ import annotations

import json
from typing import Any

from .workflow_summary import summarize_input_contracts


def print_final_state_rows_summary(rows: list[dict[str, Any]]) -> None:
    """打印 final-state 整体阶段摘要。"""

    prediction_count = len(rows)
    validation_ready_count = sum(1 for row in rows if row.get("validation_ready") is True)
    final_state_match_count = sum(1 for row in rows if row.get("final_state_match") is True)
    error_count = sum(1 for row in rows if row.get("return_code", 0) != 0)
    status = final_state_status_from_counts(
        prediction_count,
        validation_ready_count,
        final_state_match_count,
        error_count=error_count,
    )
    print(
        f"[finished final-state] {status} "
        f"predictions={prediction_count} "
        f"validation_ready={count_text(validation_ready_count, prediction_count)} "
        f"final_state_match={count_text(final_state_match_count, prediction_count)} "
        f"errors={error_count}",
        flush=True,
    )


def print_prediction_result(index: int, total: int, row: dict[str, Any]) -> None:
    """打印 final-state prediction 的简短结果行。"""

    return_code = row.get("return_code")
    validation_ready = row.get("validation_ready")
    final_state_match = row.get("final_state_match")
    if return_code != 0:
        status = "error"
    elif validation_ready is not True:
        status = "not_ready"
    elif final_state_match is True:
        status = "ok"
    elif final_state_match is False:
        status = "mismatch"
    else:
        status = "unknown"
    elapsed_sec = row.get("elapsed_sec")
    elapsed_text = f" elapsed_sec={float(elapsed_sec):.2f}" if isinstance(elapsed_sec, (int, float)) else ""
    print(
        f"[{index}/{total}] result {status} "
        f"return_code={return_code} validation_ready={progress_value(validation_ready)} "
        f"final_state_match={progress_value(final_state_match)}{elapsed_text}",
        flush=True,
    )


def progress_value(value: Any) -> str:
    """把进度行中的 Python 值转成短字符串。"""

    if isinstance(value, bool) or value is None:
        return json.dumps(value)
    return str(value)


def print_summary(stage: str, report: dict[str, Any]) -> None:
    """向终端输出短摘要。"""

    if stage == "quality":
        input_contracts = summarize_input_contracts(report)
        run_count = report.get("run_count")
        status = "ok" if report.get("quality_ready") is True else "needs_attention"
        print(
            f"[finished {stage}] {status} "
            f"runs={value_text(run_count)} "
            f"state_ready={count_text(report.get('state_quality_ready_count'), run_count)} "
            f"profile_ready={count_text(report.get('profile_quality_ready_count'), run_count)} "
            f"input_contracts={count_text(input_contracts['ready_count'], input_contracts['input_count'])}",
            flush=True,
        )
        return
    if stage == "transition":
        prediction_count = report.get("prediction_count")
        status = transition_stage_status(report)
        print(
            f"[finished {stage}] {status} "
            f"predictions={value_text(prediction_count)} "
            f"ready={count_text(report.get('ready_count'), prediction_count)} "
            f"exact={count_text(report.get('exact_count'), prediction_count)} "
            f"transition_count_exact={count_text(report.get('transition_count_exact_count'), prediction_count)}",
            flush=True,
        )
        return

    prediction_count = report.get("prediction_count")
    status = final_state_stage_status(report)
    prefix = "summary" if stage.startswith("final-state:") else "finished"
    print(
        f"[{prefix} {stage}] {status} "
        f"predictions={value_text(prediction_count)} "
        f"validation_ready={count_text(report.get('validation_ready_count'), prediction_count)} "
        f"final_state_match={count_text(report.get('final_state_match_count'), prediction_count)} "
        f"pass_rate={percent_text(report.get('final_state_pass_rate'))}",
        flush=True,
    )


def print_stage_start(stage: str, detail: str) -> None:
    """向终端输出一个人读的阶段开始行。"""

    print(f"[running {stage}] {detail}", flush=True)


def quality_stage_detail(runs: list[Any]) -> str:
    """返回 quality 阶段的人读说明。"""

    return (
        f"profile quality audit: runs={len(runs)} "
        f"inputs={len({run.input_id for run in runs})} "
        f"configs={len({run.config_id for run in runs})}"
    )


def final_state_stage_detail(specs: list[Any], *, dry_run: bool) -> str:
    """返回 final-state 阶段的人读说明。"""

    self_count = sum(1 for spec in specs if spec.is_self)
    cross_count = len(specs) - self_count
    dry_run_text = " dry_run=true" if dry_run else ""
    return (
        f"state prediction matrix: predictions={len(specs)} "
        f"self={self_count} cross={cross_count}{dry_run_text}"
    )


def final_state_stage_status(report: dict[str, Any]) -> str:
    """把 final-state summary 规整成人读状态。"""

    prediction_count = report.get("prediction_count")
    validation_ready_count = report.get("validation_ready_count")
    final_state_match_count = report.get("final_state_match_count")
    if not all(
        is_plain_int(value)
        for value in (prediction_count, validation_ready_count, final_state_match_count)
    ):
        return "unknown"
    return final_state_status_from_counts(
        prediction_count,
        validation_ready_count,
        final_state_match_count,
        error_count=0,
    )


def final_state_status_from_counts(
    prediction_count: int,
    validation_ready_count: int,
    final_state_match_count: int,
    *,
    error_count: int,
) -> str:
    """根据 final-state 计数字段返回人读状态。"""

    if prediction_count == 0:
        return "empty"
    if error_count:
        return "error"
    if validation_ready_count != prediction_count:
        return "not_ready"
    if final_state_match_count == prediction_count:
        return "ok"
    return "mismatch"


def transition_stage_status(report: dict[str, Any]) -> str:
    """把 transition summary 规整成人读状态。"""

    prediction_count = report.get("prediction_count")
    ready_count = report.get("ready_count")
    exact_count = report.get("exact_count")
    if not all(is_plain_int(value) for value in (prediction_count, ready_count, exact_count)):
        return "unknown"
    return transition_status_from_counts(prediction_count, ready_count, exact_count)


def transition_status_from_counts(
    prediction_count: int,
    ready_count: int,
    exact_count: int,
) -> str:
    """根据 transition 计数字段返回人读状态。"""

    if prediction_count == 0:
        return "empty"
    if ready_count != prediction_count:
        return "not_ready"
    if exact_count == prediction_count:
        return "ok"
    return "mismatch"


def count_text(count: Any, total: Any) -> str:
    """把 count/total 转成人读短字符串。"""

    return f"{value_text(count)}/{value_text(total)}"


def is_plain_int(value: Any) -> bool:
    """判断摘要计数字段是否为普通整数。"""

    return isinstance(value, int) and not isinstance(value, bool)


def value_text(value: Any) -> str:
    """把摘要值转成人读短字符串。"""

    return "unknown" if value is None else str(value)


def percent_text(value: Any) -> str:
    """把 0-1 pass rate 转成人读百分比。"""

    if isinstance(value, (int, float)):
        return f"{value * 100:.1f}%"
    return "unknown"
