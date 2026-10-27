"""Host-side batch call into the modeling environment; no input acquisition."""

from __future__ import annotations

from dataclasses import dataclass
import os
from pathlib import Path
import subprocess
import time
from typing import Any

from markov_internal.common.io import load_json, write_json
from markov_internal.common.paths import ROOT_DIR, repo_relative_path
from markov_internal.common.process import run_command, start_process, stop_process
from markov_internal.modeling.context import PredictionInputs
from markov_internal.modeling.types import TargetHiCacheConfig


@dataclass(frozen=True)
class PredictionOutcome:
    """This attempt's report and wall time; None means no current report exists."""

    return_code: int
    summary: dict[str, Any] | None
    wall_seconds: float
    report_path: Path
    launched: bool = True

    def charged_predictions(self, submitted: int) -> int:
        """Charge interrupted batches conservatively; their report can still be the initial checkpoint."""

        if not self.launched:
            return 0
        if self.return_code in {124, 130, 137} or self.return_code < 0 or self.summary is None:
            return submitted

        return sum(row["attempted"] for row in self.summary["prediction"]["cells"])


def invoke_prediction(
    inputs: PredictionInputs,
    targets: tuple[TargetHiCacheConfig, ...],
    output_dir: Path,
    *,
    jobs: int = 1,
    trace_threads: int = 1,
    trace_file_threads: int = 1,
    diagnostics: str = "off",
    experiment_reports: bool = False,
    dry_run: bool = False,
    max_wall_sec: float | None = None,
) -> PredictionOutcome:
    """Run the same public predictor used by direct, experiment and search calls.

    Only prepared inputs are accepted. Output belongs to this attempt; a launch
    failure never returns an older success. Times inside summary remain in us.
    """

    started = time.monotonic()
    report = output_dir / "workflow_summary.json"
    report.unlink(missing_ok=True)

    def relative(path: Path) -> str:
        return str(repo_relative_path(path))

    command = [
        str(ROOT_DIR / "scripts/model.sh"),
        "predict",
        "--cost-model",
        relative(inputs.cost_model),
        "--output-dir",
        relative(output_dir),
        "--model-run-jobs",
        str(jobs),
        "--trace-threads",
        str(trace_threads),
        "--trace-file-threads",
        str(trace_file_threads),
        "--diagnostics",
        diagnostics,
        "--continue-on-error",
    ]
    if experiment_reports:
        command.append("--experiment-reports")
    if dry_run:
        command.append("--dry-run")
    if inputs.resource_environment is not None:
        resources = output_dir / "inputs" / "resource_environment.json"
        write_json(resources, inputs.resource_environment)
        command.extend(("--resource-environment", relative(resources)))

    for source in inputs.source_manifests:
        command.extend(("--source-manifest", relative(source)))
    for service in inputs.cpu_service_costs:
        command.extend(("--cpu-service-cost", relative(service)))
    for index, target in enumerate(targets, start=1):
        if max_wall_sec is not None and time.monotonic() - started >= max_wall_sec:
            return PredictionOutcome(124, None, time.monotonic() - started, report, launched=False)

        path = output_dir / "inputs" / f"target_{index}.json"
        write_json(path, {"name": target.label, "hicache": target.fields})
        command.extend(("--target-config", relative(path)))

    if max_wall_sec is None:
        return_code = run_command(command).returncode
    else:
        # A dedicated container bounds startup as well as native source/target
        # work. Killing only the compose client can leave its container running.
        name = f"markov-prediction-{os.getpid()}-{time.time_ns()}"
        log_path = output_dir / "invocation.log"
        process = start_process(command, log_path, {**os.environ, "TRACE_SIM_RUN_CONTAINER_NAME": name})
        interrupted = False
        try:
            return_code = process.wait(timeout=max(0.0, max_wall_sec - (time.monotonic() - started)))
        except (subprocess.TimeoutExpired, KeyboardInterrupt) as error:
            return_code = 124 if isinstance(error, subprocess.TimeoutExpired) else 130
            interrupted = True
        finally:
            stop_process(process, timeout_sec=2)
            if interrupted:
                subprocess.run(["docker", "rm", "--force", name], capture_output=True, timeout=20, check=False)

        if return_code == 0:
            log_path.unlink(missing_ok=True)

    summary = load_json(report) if report.is_file() else None
    return PredictionOutcome(return_code, summary, time.monotonic() - started, report)
