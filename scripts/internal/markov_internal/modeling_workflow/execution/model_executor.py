"""执行统一的 ModelRunSpec 计划。"""

from __future__ import annotations

import concurrent.futures
import subprocess
import time
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any

from ...common.io import write_json
from ...common.paths import ROOT_DIR
from ..artifacts import ModelRunArtifacts
from ..progress import count_text
from ..types import ModelRunResult, ModelRunSpec, ModelOutputRequirement
from .runner_adapter import RunnerConfigBuilder

if TYPE_CHECKING:
    from ..context import WorkflowContext


@dataclass
class ModelRunExecutor:
    """执行、复用或跳过一组 ModelRunSpec。"""

    context: "WorkflowContext"
    specs: list[ModelRunSpec]
    rows: list[dict[str, Any]] = field(default_factory=list)
    results: dict[str, ModelRunResult] = field(default_factory=dict)

    def run(self) -> dict[str, ModelRunResult]:
        """执行完整 model-runs 阶段。"""

        progress = self.context.reporter.start_stage(
            "model-runs",
            len(self.specs),
            f"validations {','.join(self.context.options.validations)} | jobs {self.context.options.model_run_jobs}",
            unit="run",
        )
        if self.context.options.model_run_jobs > 1:
            return self._run_parallel(progress)
        return self._run_serial(progress)

    def _run_serial(self, progress: Any) -> dict[str, ModelRunResult]:
        """顺序执行 model-runs。"""

        for spec in self.specs:
            result = self._run_one(spec)
            self._record_result(result, progress)
            self._raise_if_failed(result, progress)
        summary = self._write_summary()
        progress.finish(self._status(summary), self._done_text())
        return self.results

    def _run_parallel(self, progress: Any) -> dict[str, ModelRunResult]:
        """并发执行 model-runs。"""

        max_workers = min(self.context.options.model_run_jobs, max(1, len(self.specs)))
        stop_after_failure = False
        with concurrent.futures.ThreadPoolExecutor(max_workers=max_workers) as executor:
            futures: dict[concurrent.futures.Future[ModelRunResult], ModelRunSpec] = {}
            pending = iter(self.specs)
            while len(futures) < max_workers:
                try:
                    spec = next(pending)
                except StopIteration:
                    break
                futures[executor.submit(self._run_one, spec)] = spec
            while futures:
                done, _pending = concurrent.futures.wait(
                    futures,
                    return_when=concurrent.futures.FIRST_COMPLETED,
                )
                for future in done:
                    spec = futures.pop(future)
                    if future.cancelled():
                        continue
                    result = future.result()
                    self._record_result(result, progress, inflight=len(futures))
                    if result.return_code != 0 and not self.context.options.continue_on_error:
                        stop_after_failure = True
                        continue
                    if stop_after_failure:
                        continue
                    try:
                        next_spec = next(pending)
                    except StopIteration:
                        continue
                    futures[executor.submit(self._run_one, next_spec)] = next_spec
                if stop_after_failure:
                    for future in list(futures):
                        if future.cancel():
                            futures.pop(future, None)
        summary = self._write_summary()
        if summary.get("error_count") and not self.context.options.continue_on_error:
            progress.finish("ERROR", self._done_text())
            failed = next((result for result in self.results.values() if result.return_code != 0), None)
            if failed is not None:
                raise SystemExit(f"Model run failed: {failed.spec.label}; see {failed.artifacts.model_log}")
            raise SystemExit("Model run failed.")
        progress.finish(self._status(summary), self._done_text())
        return self.results

    def _record_result(self, result: ModelRunResult, progress: Any, *, inflight: int = 0) -> None:
        """记录一次执行结果并推进进度。"""

        self.results[result.spec.run_id] = result
        self.rows.append(self._row(result))
        progress.advance(self._running_metrics(inflight=inflight))

    def _raise_if_failed(self, result: ModelRunResult, progress: Any) -> None:
        """在非 continue-on-error 模式下处理失败。"""

        if result.return_code == 0 or self.context.options.continue_on_error:
            return
        self._write_summary()
        progress.finish("ERROR", self._done_text())
        raise SystemExit(f"Model run failed: {result.spec.label}; see {result.artifacts.model_log}")

    def _run_one(self, spec: ModelRunSpec) -> ModelRunResult:
        spec.output_dir.mkdir(parents=True, exist_ok=True)
        artifacts = ModelRunArtifacts.from_spec(spec)
        runner_config_path = self.context.artifacts.runner_config_path(spec.run_id)
        builder = RunnerConfigBuilder(spec)
        builder.write_config(runner_config_path)
        command = builder.command(runner_config_path)
        write_json(artifacts.command_json, {"command": command, "label": spec.label, "model_run_id": spec.run_id})

        if spec.skip_reason:
            return ModelRunResult(
                spec=spec,
                return_code=0,
                elapsed_sec=0.0,
                artifacts=artifacts,
                skipped=True,
                skip_reason=spec.skip_reason,
            )
        if self.context.options.dry_run:
            return ModelRunResult(
                spec=spec,
                return_code=0,
                elapsed_sec=0.0,
                artifacts=artifacts,
                skipped=True,
                dry_run=True,
                skip_reason="dry_run",
            )
        if not self.context.options.force and primary_artifact_ready(spec, artifacts):
            return ModelRunResult(spec=spec, return_code=0, elapsed_sec=0.0, artifacts=artifacts)

        return_code, elapsed_sec = execute_command(command, artifacts.model_log)
        error_tail = read_log_tail(artifacts.model_log) if return_code != 0 else ""
        return ModelRunResult(
            spec=spec,
            return_code=return_code,
            elapsed_sec=elapsed_sec,
            artifacts=artifacts,
            execution_error_tail=error_tail,
        )

    def _row(self, result: ModelRunResult) -> dict[str, Any]:
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
            "output_requirements": sorted(requirement.value for requirement in spec.output_requirements),
            "output_dir": str(spec.output_dir),
            "log_path": str(result.artifacts.model_log),
            "return_code": result.return_code,
            "elapsed_sec": result.elapsed_sec,
            "skipped": result.skipped,
            "dry_run": result.dry_run,
            "skip_reason": result.skip_reason or None,
        }
        if result.execution_error_tail:
            row["execution_error_tail"] = result.execution_error_tail
        return row

    def _running_metrics(self, *, inflight: int = 0) -> dict[str, Any]:
        stats = self._stats()
        metrics: dict[str, Any] = {"usable": count_text(stats["usable_count"], stats["runnable_count"])}
        if self.context.options.model_run_jobs > 1:
            metrics["inflight"] = str(inflight)
        if stats["skipped_count"]:
            metrics["skipped"] = str(stats["skipped_count"])
        if stats["error_count"]:
            metrics["errors"] = str(stats["error_count"])
        return metrics

    def _write_summary(self) -> dict[str, Any]:
        stats = self._stats()
        order = {spec.run_id: index for index, spec in enumerate(self.specs)}
        rows = sorted(self.rows, key=lambda row: order.get(str(row.get("model_run_id")), len(order)))
        summary = {
            "schema": "trace_sim.modeling_workflow.model_runs.v1",
            "run_count": len(rows),
            **stats,
            "rows": rows,
        }
        write_json(self.context.artifacts.model_runs_summary_path, summary)
        return summary

    def _status(self, summary: dict[str, Any]) -> str:
        if summary.get("run_count") == 0:
            return "EMPTY"
        if summary.get("error_count"):
            return "ERROR"
        if summary.get("skipped_count"):
            return "CHECK"
        return "OK"

    def _done_text(self) -> str:
        stats = self._stats()
        text = f"{len(self.rows)} runs | usable {count_text(stats['usable_count'], stats['runnable_count'])}"
        if stats["skipped_count"]:
            text += f" | skipped {stats['skipped_count']}"
        if stats["dry_run_count"]:
            text += f" | dry-run {stats['dry_run_count']}"
        if stats["error_count"]:
            text += f" | errors {stats['error_count']}"
        return text

    def _stats(self) -> dict[str, int]:
        runnable_count = sum(1 for row in self.rows if not row.get("skipped"))
        usable_count = sum(1 for row in self.rows if not row.get("skipped") and row.get("return_code") == 0)
        return {
            "handled_count": len(self.rows),
            "runnable_count": runnable_count,
            "usable_count": usable_count,
            "error_count": sum(
                1 for row in self.rows if not row.get("skipped") and row.get("return_code") not in (0, None)
            ),
            "skipped_count": sum(1 for row in self.rows if row.get("skipped")),
            "dry_run_count": sum(1 for row in self.rows if row.get("dry_run")),
        }


def primary_artifact_ready(spec: ModelRunSpec, artifacts: ModelRunArtifacts) -> bool:
    """判断已有产物是否足够复用。"""

    if ModelOutputRequirement.MODULE_VALIDATION in spec.output_requirements:
        return artifacts.validation_json.is_file()
    if (
        ModelOutputRequirement.MODULE_SUMMARY in spec.output_requirements
        or ModelOutputRequirement.MODULE_TRANSITION_DIAGNOSTICS in spec.output_requirements
    ):
        return artifacts.model_summary_json.is_file()
    if ModelOutputRequirement.BASE_DAG_DIAGNOSTICS in spec.output_requirements:
        return artifacts.dag_quality_json.is_file()
    return artifacts.run_summary_json.is_file()


def execute_command(command: list[str], log_path: Any) -> tuple[int, float]:
    """执行建模命令并把 stdout/stderr 写入日志。"""

    log_path.parent.mkdir(parents=True, exist_ok=True)
    start = time.monotonic()
    with log_path.open("w", encoding="utf-8") as log_file:
        completed = subprocess.run(
            command,
            cwd=ROOT_DIR,
            stdout=log_file,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
    return completed.returncode, time.monotonic() - start


def read_log_tail(path: Any, limit: int = 8000) -> str:
    """读取有限长度的命令日志尾部。"""

    if not path.is_file():
        return ""
    return path.read_text(encoding="utf-8", errors="replace")[-limit:]
