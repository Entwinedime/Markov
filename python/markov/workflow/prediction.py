"""Run one prepared prediction session, with base and costs isolated from evaluator data."""

from dataclasses import asdict
import fcntl
import os
from pathlib import Path
import subprocess
import time

from markov.artifacts.request import PredictionRequest, read_json
from markov.artifacts.performance import encoded
from markov.artifacts.workload import Workload
from markov.artifacts.writing import write_json
from markov.instrumentation.containers import container_state
from markov.workflow.capture import InferenceCapture
from markov.workflow.performance import select_models
from markov.workflow.preparation import PerformancePreparation


def predict(request: PredictionRequest) -> dict:
    output = request.execution.output
    if request.base.manifest is not None and output.is_relative_to(request.base.manifest.parent):
        raise ValueError("Prediction output must be separate from the base capture")
    if request.models is not None and request.models.is_relative_to(output / "prediction"):
        raise ValueError("Prediction output must be separate from selected models")
    if request.database is not None and request.database.is_relative_to(output / "prediction"):
        raise ValueError("Prediction output must not expose the performance database")
    request.execution.output.mkdir(parents=True, exist_ok=True)
    with (request.execution.output / "prediction.lock").open("a") as lease:
        fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
        return _predict(request)


def _predict(request: PredictionRequest) -> dict:
    begin = time.monotonic()
    if request.execution.kernel_cache_initial_state is None:
        raise ValueError("Prediction requires execution.kernel_cache_initial_state: empty or prepared")
    output = request.execution.output / "prediction"
    process = PredictionProcess(output, request.execution.memory_limit_gib or 4)
    process.check_available()
    models = select_models(
        request.deployment,
        request.models,
        request.database,
        request.execution.output / "performance",
        request.sampling_budget is not None,
    )
    performance_sec = time.monotonic() - begin
    workload = Workload.load(request.workload)
    capture_begin = time.monotonic()
    base = InferenceCapture(request, workload=workload).run(reuse=True)
    capture_sec = time.monotonic() - capture_begin
    if base["status"] not in ("completed", "reused"):
        return base

    manifest = Path(base["manifest"])
    output.mkdir(parents=True, exist_ok=True)
    prepared = request.execution.output / "prediction_input.json"
    inputs = {
        "manifest": str(manifest),
        "models": str(models),
        "deployment": asdict(request.deployment),
        "workload": asdict(workload),
        "targets": [asdict(target) for target in request.targets],
        "kernel_cache_initial_state": request.execution.kernel_cache_initial_state,
    }
    seen = set()
    preparation = None
    while True:
        inputs["models"] = str(models)
        write_json(prepared, inputs)
        result = process.run(manifest, models, prepared)
        if result["status"] == "failed":
            return result
        requirements = {}
        for cell in result["cells"]:
            for gap in cell["cost_gaps"]:
                entry = requirements.setdefault(encoded(gap), {**gap, "targets": []})
                entry["targets"].append(cell["name"])
        result["cost_requirements"] = list(requirements.values())
        if not requirements or request.sampling_budget is None:
            break
        missing = tuple(sorted(requirements))
        if missing in seen:
            result["preparation_issue"] = (
                "The same work requirements remain after shared preparation; no further capture was started"
            )
            break
        seen.add(missing)
        performance_begin = time.monotonic()
        if preparation is None:
            preparation = PerformancePreparation(request)
        next_models = preparation.prepare(result, models)
        performance_sec += time.monotonic() - performance_begin
        if next_models is None:
            break
        models = next_models
    result["preparation"] = {
        "base_status": base["status"],
        "base_sec": capture_sec,
        "performance_source": "models" if request.models is not None else "database",
        "performance_sec": performance_sec,
        "sampling_rounds": preparation.rounds if preparation is not None else [],
        "sampling_state": str(preparation.task.state_path) if preparation is not None else None,
    }
    result["workflow_sec"] = time.monotonic() - begin
    write_json(process.report, result)
    result["report"] = str(process.report)
    return result


class PredictionProcess:
    """Own the isolated modeling container; no database, device or evaluator mounts."""

    def __init__(self, output: Path, memory_limit_gib: int):
        self.output = output
        self.memory_limit_gib = memory_limit_gib
        self.cid = output / "container.id"
        self.report = output / "report.json"

    def check_available(self) -> None:
        current = container_state(self.cid)
        if current is not None and current["Running"]:
            raise ValueError("This prediction output still has a running container")
        self.report.with_name(self.report.name + ".partial").unlink(missing_ok=True)

    def run(self, manifest: Path, models: Path, prepared: Path) -> dict:
        executable = Path(__file__).resolve().parents[3] / "build/modeling/Release/markov_core"
        self.cid.unlink(missing_ok=True)
        args = [
            "docker",
            "run",
            "--rm",
            "--init",
            "--network",
            "none",
            "--pids-limit",
            "256",
            "--cidfile",
            str(self.cid),
            "--memory",
            f"{self.memory_limit_gib}g",
            "--memory-swap",
            f"{self.memory_limit_gib}g",
        ]

        # No repository/data-root mount: the evaluator's normal observations and
        # target traces cannot enter this container, even as unused neighbors.
        for source, target, writable in (
            (executable, "/opt/markov/markov_core", False),
            (manifest.parent, str(manifest.parent), False),
            (models, str(models), False),
            (prepared, str(prepared), False),
            (self.output, str(self.output), True),
        ):
            args += ["--mount", f"type=bind,source={source},target={target}" + ("" if writable else ",readonly")]
        args += [
            os.environ.get("MARKOV_MODELING_IMAGE", "markov-modeling:ubuntu24.04"),
            "/opt/markov/markov_core",
            "predict",
            str(prepared),
            str(self.report),
        ]
        write_json(self.output / "invocation.json", args)
        with (self.output / "run.log").open("w", encoding="utf-8") as log:
            process = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT)
            try:
                code = process.wait()
            finally:
                if process.poll() is None:
                    if self.cid.exists():
                        subprocess.run(
                            ["docker", "rm", "-f", self.cid.read_text().strip()], stdout=log, stderr=log, timeout=20
                        )
                    process.terminate()
                    process.wait(timeout=20)
        if code:
            return {"status": "failed", "exit_code": code, "log": str(self.output / "run.log")}
        return read_json(self.report)
