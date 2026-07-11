"""Execution, reuse, and persistence of normalized model-run cells."""

from __future__ import annotations

import concurrent.futures
import json
import time
from collections.abc import Iterator
from dataclasses import dataclass, field
from pathlib import Path
from typing import TYPE_CHECKING, Any

from ...common.io import load_json, write_json
from ...common.process import run_command
from ..artifacts import ModelRunArtifacts
from ..progress import StageProgress, count_text
from ..types import ModelOutputRequirement, ModelRunCounts, ModelRunResult, ModelRunSpec
from .runner_adapter import RunnerConfigBuilder

if TYPE_CHECKING:
    from ..context import WorkflowContext


@dataclass
class ModelRunExecutor:
    """Execute, reuse, dry-run, or skip every cell in a model-run plan."""

    context: WorkflowContext
    specs: list[ModelRunSpec]
    rows: list[dict[str, Any]] = field(default_factory=list)
    results: dict[str, ModelRunResult] = field(default_factory=dict)

    def run(self) -> dict[str, ModelRunResult]:
        """Execute the plan, persist status rows, and enforce failure policy.

        Results are recorded as workers finish. When `continue_on_error` is false,
        no new parallel work is scheduled after the first failure and the method
        raises `SystemExit` after writing the aggregate summary.
        """

        detail = f"jobs {self.context.options.model_run_jobs}" if self.context.options.model_run_jobs > 1 else ""
        progress = self.context.reporter.start_stage("model-runs", len(self.specs), detail, unit="run")
        first_failure: ModelRunResult | None = None

        for result, inflight in self._result_stream():
            self._record_result(result, progress, inflight=inflight)
            if result.return_code != 0 and first_failure is None:
                first_failure = result

        summary = self._write_summary()
        progress.finish(model_run_status(summary), self._done_text())
        if first_failure is not None and not self.context.options.continue_on_error:
            raise SystemExit(f"Model run failed: {first_failure.spec.label}; see {first_failure.artifacts.model_log}")
        return self.results

    def _result_stream(self) -> Iterator[tuple[ModelRunResult, int]]:
        if self.context.options.model_run_jobs <= 1:
            for spec in self.specs:
                result = self._run_one(spec)
                yield result, 0
                if result.return_code != 0 and not self.context.options.continue_on_error:
                    return
            return
        yield from self._parallel_results()

    def _parallel_results(self) -> Iterator[tuple[ModelRunResult, int]]:
        max_workers = min(self.context.options.model_run_jobs, max(1, len(self.specs)))
        remaining = iter(self.specs)
        stop_scheduling = False

        with concurrent.futures.ThreadPoolExecutor(max_workers=max_workers) as executor:
            futures: dict[concurrent.futures.Future[ModelRunResult], ModelRunSpec] = {}

            def submit_next() -> bool:
                try:
                    spec = next(remaining)
                except StopIteration:
                    return False
                futures[executor.submit(self._run_one, spec)] = spec
                return True

            while len(futures) < max_workers and submit_next():
                pass

            while futures:
                done, _ = concurrent.futures.wait(
                    futures,
                    return_when=concurrent.futures.FIRST_COMPLETED,
                )
                for future in done:
                    futures.pop(future)
                    result = future.result()
                    if result.return_code != 0 and not self.context.options.continue_on_error:
                        stop_scheduling = True
                    if not stop_scheduling:
                        submit_next()
                    yield result, len(futures)

    def _record_result(self, result: ModelRunResult, progress: StageProgress, *, inflight: int) -> None:
        self.results[result.spec.run_id] = result
        row = model_run_row(result)
        self.rows.append(row)
        write_json(result.artifacts.execution_json, row)
        progress.advance(self._running_metrics(inflight=inflight))

    def _run_one(self, spec: ModelRunSpec) -> ModelRunResult:
        artifacts = ModelRunArtifacts.from_spec(spec)
        if spec.skip_reason:
            return skipped_result(spec, artifacts, spec.skip_reason)

        try:
            spec.output_dir.mkdir(parents=True, exist_ok=True)
            builder = RunnerConfigBuilder(spec)
            runner_config = builder.payload()
            if self.context.options.dry_run:
                write_json(artifacts.runner_config_json, runner_config)
                return skipped_result(spec, artifacts, "dry_run", dry_run=True)
            if not self.context.options.force and reusable_artifacts_ready(spec, artifacts, runner_config):
                return ModelRunResult(spec=spec, return_code=0, elapsed_sec=0.0, artifacts=artifacts, reused=True)

            write_json(artifacts.runner_config_json, runner_config)
            return_code, elapsed_sec = execute_command(
                builder.command(artifacts.runner_config_json), artifacts.model_log
            )
            error_tail = read_log_tail(artifacts.model_log) if return_code != 0 else ""
            return ModelRunResult(
                spec=spec,
                return_code=return_code,
                elapsed_sec=elapsed_sec,
                artifacts=artifacts,
                execution_error_tail=error_tail,
            )
        except Exception as error:
            write_internal_error(artifacts.model_log, error)
            return ModelRunResult(
                spec=spec,
                return_code=1,
                elapsed_sec=0.0,
                artifacts=artifacts,
                execution_error_tail=str(error),
            )

    def _running_metrics(self, *, inflight: int) -> dict[str, Any]:
        counts = self._counts()
        metrics: dict[str, Any] = {"usable": count_text(counts.usable, counts.runnable)}
        if self.context.options.model_run_jobs > 1:
            metrics["inflight"] = str(inflight)
        if counts.skipped:
            metrics["skipped"] = str(counts.skipped)
        if counts.reused:
            metrics["reused"] = str(counts.reused)
        if counts.errors:
            metrics["errors"] = str(counts.errors)
        return metrics

    def _write_summary(self) -> dict[str, Any]:
        summary = {
            "schema": "trace_sim.modeling_workflow.model_runs.v1",
            "run_count": len(self.rows),
            **self._counts().as_payload(),
            "total_elapsed_sec": sum(float(row.get("elapsed_sec") or 0.0) for row in self.rows),
        }
        write_json(self.context.artifacts.model_runs_summary_path, summary)
        return summary

    def _done_text(self) -> str:
        counts = self._counts()
        parts = [f"{counts.handled} runs", f"usable {count_text(counts.usable, counts.runnable)}"]
        if counts.skipped:
            parts.append(f"skipped {counts.skipped}")
        if counts.dry_run:
            parts.append(f"dry-run {counts.dry_run}")
        if counts.reused:
            parts.append(f"reused {counts.reused}")
        if counts.errors:
            parts.append(f"errors {counts.errors}")
        return " | ".join(parts)

    def _counts(self) -> ModelRunCounts:
        return ModelRunCounts.from_results(list(self.results.values()))


def skipped_result(
    spec: ModelRunSpec,
    artifacts: ModelRunArtifacts,
    reason: str,
    *,
    dry_run: bool = False,
) -> ModelRunResult:
    """Construct a successful non-execution result with a stable skip reason."""

    return ModelRunResult(
        spec=spec,
        return_code=0,
        elapsed_sec=0.0,
        artifacts=artifacts,
        skipped=True,
        dry_run=dry_run,
        skip_reason=reason,
    )


def model_run_row(result: ModelRunResult) -> dict[str, Any]:
    """Serialize one model-run result without embedding large backend artifacts."""

    spec = result.spec
    row = {
        "model_run_id": spec.run_id,
        "label": spec.label,
        "mode": spec.mode,
        "source_run_id": spec.source_profile.run_id,
        "source_config_id": spec.source_profile.config_id,
        "input_id": spec.source_profile.input_id,
        "target_run_id": spec.target_profile.run_id if spec.target_profile is not None else None,
        "target_config_id": spec.target_profile.config_id if spec.target_profile is not None else None,
        "validation_requests": list(spec.validation_requests),
        "output_requirements": list(spec.output_requirement_names),
        "output_dir": str(spec.output_dir),
        "log_path": str(result.artifacts.model_log),
        "return_code": result.return_code,
        "elapsed_sec": result.elapsed_sec,
        "skipped": result.skipped,
        "dry_run": result.dry_run,
        "reused": result.reused,
        "skip_reason": result.skip_reason or None,
    }
    if result.execution_error_tail:
        row["execution_error_tail"] = result.execution_error_tail
    return row


def reusable_artifacts_ready(
    spec: ModelRunSpec,
    artifacts: ModelRunArtifacts,
    expected_runner_config: dict[str, Any],
) -> bool:
    """Return whether existing outputs exactly match the requested runner input.

    File existence alone is insufficient: changing threads, trace channels,
    target config, oracle paths, or page-key semantics must invalidate the old
    model cell. The previous execution record must also describe a successful,
    non-skipped invocation.
    """

    required = [artifacts.run_summary_json, artifacts.prediction_json]
    if ModelOutputRequirement.HICACHE_VALIDATION in spec.output_requirements:
        required.extend(
            [
                artifacts.validation_json,
                artifacts.model_summary_json,
                artifacts.predicted_state_trace_json,
            ]
        )
    if ModelOutputRequirement.DAG_ANALYSIS in spec.output_requirements:
        required.extend(
            [
                artifacts.dag_quality_json,
                artifacts.dag_analysis_json,
                artifacts.dag_anchor_coverage_json,
                artifacts.dag_operation_visibility_json,
            ]
        )
    if not all(path.is_file() for path in required):
        return False
    try:
        existing_config = load_json(artifacts.runner_config_json)
        execution = load_json(artifacts.execution_json)
    except (OSError, json.JSONDecodeError):
        return False
    return (
        existing_config == expected_runner_config
        and isinstance(execution, dict)
        and execution.get("return_code") == 0
        and execution.get("skipped") is not True
        and execution.get("dry_run") is not True
    )


def execute_command(command: list[str], log_path: Path) -> tuple[int, float]:
    """Execute one model command and route all output to its private log."""

    start = time.monotonic()
    completed = run_command(command, log_path=log_path)
    return completed.returncode, time.monotonic() - start


def write_internal_error(path: Path, error: Exception) -> None:
    """Persist an exception raised before the external model command completed."""

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(f"internal model runner error: {error}\n", encoding="utf-8")


def read_log_tail(path: Path, limit: int = 8_000) -> str:
    """Read a bounded failure-log tail for the aggregate execution record."""

    if not path.is_file():
        return ""
    return path.read_text(encoding="utf-8", errors="replace")[-limit:]


def model_run_status(summary: dict[str, Any]) -> str:
    """Map aggregate execution counts to the workflow progress status vocabulary."""

    if summary.get("run_count") == 0:
        return "EMPTY"
    if summary.get("error_count"):
        return "ERROR"
    if summary.get("skipped_count"):
        return "CHECK"
    return "OK"
