"""执行统一的 ModelRunSpec 计划。"""

from __future__ import annotations

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
            f"validations {','.join(self.context.options.validations)}",
            unit="run",
        )
        for spec in self.specs:
            result = self._run_one(spec)
            self.results[spec.run_id] = result
            self.rows.append(self._row(result))
            progress.advance(self._running_metrics())
            if result.return_code != 0 and not self.context.options.continue_on_error:
                self._write_summary()
                progress.finish("ERROR", self._done_text())
                raise SystemExit(f"Model run failed: {spec.label}; see {result.artifacts.model_log}")
        summary = self._write_summary()
        progress.finish(self._status(summary), self._done_text())
        return self.results

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

    def _running_metrics(self) -> dict[str, Any]:
        total = len(self.rows)
        return {
            "ok": count_text(sum(1 for row in self.rows if row.get("return_code") == 0), total),
            "skipped": str(sum(1 for row in self.rows if row.get("skipped"))),
        }

    def _write_summary(self) -> dict[str, Any]:
        summary = {
            "schema": "trace_sim.modeling_workflow.model_runs.v1",
            "run_count": len(self.rows),
            "ok_count": sum(1 for row in self.rows if row.get("return_code") == 0),
            "error_count": sum(1 for row in self.rows if row.get("return_code") not in (0, None)),
            "skipped_count": sum(1 for row in self.rows if row.get("skipped")),
            "dry_run_count": sum(1 for row in self.rows if row.get("dry_run")),
            "rows": self.rows,
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
        total = len(self.rows)
        ok_count = sum(1 for row in self.rows if row.get("return_code") == 0)
        skipped_count = sum(1 for row in self.rows if row.get("skipped"))
        error_count = sum(1 for row in self.rows if row.get("return_code") not in (0, None))
        text = f"{total} runs | ok {count_text(ok_count, total)}"
        if skipped_count:
            text += f" | skipped {skipped_count}"
        if error_count:
            text += f" | errors {error_count}"
        return text


def primary_artifact_ready(spec: ModelRunSpec, artifacts: ModelRunArtifacts) -> bool:
    """判断已有产物是否足够复用。"""

    if ModelOutputRequirement.MODULE_VALIDATION in spec.output_requirements:
        return artifacts.validation_json.is_file()
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
