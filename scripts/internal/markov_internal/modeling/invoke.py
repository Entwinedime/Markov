"""Host-side batch call into the modeling environment; no input acquisition."""

from __future__ import annotations

from dataclasses import dataclass, replace
import json
import os
import selectors
from pathlib import Path
import subprocess
import time
from typing import Any

from markov_internal.common.io import load_json, truncate_log_tail, write_json
from markov_internal.common.paths import ROOT_DIR, repo_relative_path
from markov_internal.common.process import stop_process
from markov_internal.common.progress import StageProgress
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


class PredictionClient:
    """Own one active fixed-input container; changing inputs releases it first."""

    def __init__(
        self,
        directory: Path,
        *,
        jobs: int = 1,
        trace_threads: int = 1,
        trace_file_threads: int = 1,
        diagnostics: str = "off",
        experiment_reports: bool = False,
        dry_run: bool = False,
    ) -> None:
        self.inputs: PredictionInputs | None = None
        self.root = directory
        self.directory = directory
        self.invocations = 0
        self.execution = dict(
            jobs=jobs,
            trace_threads=trace_threads,
            trace_file_threads=trace_file_threads,
            diagnostics=diagnostics,
            experiment_reports=experiment_reports,
            dry_run=dry_run,
        )
        self.process: subprocess.Popen[bytes] | None = None
        self.container = (
            os.environ.get("TRACE_SIM_RUN_CONTAINER_NAME") or f"markov-prediction-{os.getpid()}-{time.time_ns()}"
        )
        self.log = None
        self.failed = False
        self.last_close_wall_seconds = 0.0

    def __enter__(self) -> PredictionClient:
        return self

    def __exit__(self, *exc: Any) -> None:
        self.close()

    def _start(self) -> None:
        self.failed = False
        self.directory = self.root / str(self.invocations)
        self.invocations += 1
        self.container = (
            os.environ.get("TRACE_SIM_RUN_CONTAINER_NAME") or f"markov-prediction-{os.getpid()}-{time.time_ns()}"
        )
        request = self.directory / "session_inputs.json"
        write_json(
            request,
            {
                "source_manifests": [str(repo_relative_path(path)) for path in self.inputs.source_manifests],
                "cpu_service_costs": [str(repo_relative_path(path)) for path in self.inputs.cpu_service_costs],
                "cost_model": str(repo_relative_path(self.inputs.cost_model)),
                "resource_environment": self.inputs.resource_environment,
                "directory": str(repo_relative_path(self.directory)),
                "execution": self.execution,
            },
        )
        self.log = (self.directory / "session.log").open("w", encoding="utf-8")
        self.log.write(f"Prediction session container: {self.container}\n")
        self.log.flush()
        self.process = subprocess.Popen(
            [
                str(ROOT_DIR / "scripts/run.sh"),
                "modeling",
                "--",
                "env",
                "PYTHONPATH=scripts/internal",
                "python3",
                "-m",
                "markov_internal.modeling.session",
                "--request",
                str(repo_relative_path(request)),
            ],
            cwd=ROOT_DIR,
            env={**os.environ, "TRACE_SIM_RUN_CONTAINER_NAME": self.container},
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self.log,
            bufsize=0,
            start_new_session=True,
        )
        os.set_blocking(self.process.stdin.fileno(), False)

    def predict(
        self,
        inputs: PredictionInputs,
        targets: tuple[TargetHiCacheConfig, ...],
        output_dir: Path,
        *,
        max_wall_sec: float | None = None,
    ) -> PredictionOutcome:
        started = time.monotonic()
        report = output_dir / "workflow_summary.json"
        report.unlink(missing_ok=True)
        if max_wall_sec is not None and max_wall_sec <= 0:
            return PredictionOutcome(124, None, 0.0, report, launched=False)

        code = 1
        launched = False
        progress = StageProgress("prediction call", len(targets), unit="target")
        try:
            if inputs != self.inputs:
                self.close()
                self.inputs = PredictionInputs(
                    inputs.source_manifests,
                    inputs.cost_model,
                    inputs.cpu_service_costs,
                    dict(inputs.resource_environment) if inputs.resource_environment is not None else None,
                )
            if self.process is None:
                self._start()
            message = memoryview(
                (
                    json.dumps(
                        {
                            "targets": [{"name": target.label, "hicache": target.fields} for target in targets],
                            "output_dir": str(repo_relative_path(output_dir)),
                        },
                        allow_nan=False,
                    )
                    + "\n"
                ).encode()
            )
            with selectors.DefaultSelector() as ready:
                ready.register(self.process.stdin, selectors.EVENT_WRITE)
                while message:
                    remaining = None if max_wall_sec is None else max(0.0, max_wall_sec - (time.monotonic() - started))
                    if not ready.select(remaining):
                        raise subprocess.TimeoutExpired("prediction input", max_wall_sec)
                    message = message[os.write(self.process.stdin.fileno(), message) :]
                launched = True
                ready.unregister(self.process.stdin)
                ready.register(self.process.stdout, selectors.EVENT_READ)
                response_bytes = bytearray()
                while not response_bytes.endswith(b"\n"):
                    remaining = None if max_wall_sec is None else max(0.0, max_wall_sec - (time.monotonic() - started))
                    if not ready.select(remaining):
                        raise subprocess.TimeoutExpired("prediction session", max_wall_sec)
                    chunk = os.read(self.process.stdout.fileno(), 65536)
                    if not chunk:
                        raise RuntimeError("modeling container ended before returning the current batch")
                    response_bytes.extend(chunk)
            response = json.loads(response_bytes)
            code = response["return_code"]
            self.failed |= code != 0
        except (subprocess.TimeoutExpired, KeyboardInterrupt) as error:
            code = 124 if isinstance(error, subprocess.TimeoutExpired) else 130
            self.failed = True
            self.close(force=True)
        except Exception as error:
            self.failed = True
            if self.log is not None:
                self.log.write(f"Prediction session failed: {error}\n")
                self.log.flush()
            self.close(force=True)
        summary = load_json(report) if report.is_file() else None
        complete = summary["prediction"]["completed_count"] if summary is not None else 0
        progress.finish("OK" if code == 0 else "CHECK", f"{complete}/{len(targets)} predicted | code {code}")
        return PredictionOutcome(code, summary, time.monotonic() - started, report, launched=launched)

    def close(self, *, force: bool = False) -> None:
        started = time.monotonic()
        if self.process is not None:
            try:
                if force:
                    subprocess.run(["docker", "rm", "--force", self.container], capture_output=True, timeout=20)
                else:
                    self.process.stdin.close()
                    if self.process.wait(timeout=5) != 0:
                        self.failed = True
                        subprocess.run(["docker", "rm", "--force", self.container], capture_output=True, timeout=20)
            except (BrokenPipeError, subprocess.TimeoutExpired):
                subprocess.run(["docker", "rm", "--force", self.container], capture_output=True, timeout=20)
            finally:
                stop_process(self.process, timeout_sec=2)
                self.process.stdout.close()
                if not self.process.stdin.closed:
                    self.process.stdin.close()
                self.process = None
        if self.log is not None:
            path = Path(self.log.name)
            self.log.close()
            self.log = None
            if not self.failed and self.execution["diagnostics"] == "off":
                path.unlink(missing_ok=True)
            else:
                truncate_log_tail(path, 16 * 1024 * 1024 if self.execution["diagnostics"] == "full" else 64 * 1024)
        self.last_close_wall_seconds = time.monotonic() - started


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
    """One-shot convenience call; multi-batch applications own PredictionClient."""

    started = time.monotonic()
    with PredictionClient(
        output_dir / "session",
        jobs=jobs,
        trace_threads=trace_threads,
        trace_file_threads=trace_file_threads,
        diagnostics=diagnostics,
        experiment_reports=experiment_reports,
        dry_run=dry_run,
    ) as session:
        outcome = session.predict(inputs, targets, output_dir, max_wall_sec=max_wall_sec)
    return replace(outcome, wall_seconds=time.monotonic() - started)
