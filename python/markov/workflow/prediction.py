"""Run one prepared prediction session, with base and costs isolated from evaluator data."""

from dataclasses import asdict
import fcntl
from pathlib import Path
import time

from markov.artifacts.request import PredictionRequest
from markov.artifacts.performance import encoded
from markov.artifacts.workload import Workload
from markov.artifacts.writing import write_json
from markov.instrumentation.modeling import ModelingProcess
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
    process = ModelingProcess(output, request.execution.memory_limit_gib or 4)
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
        "node_scale": [asdict(rule) for rule in request.execution.node_scale],
        "record_graph": request.execution.record_graph,
    }
    seen = set()
    preparation = None
    while True:
        inputs["models"] = str(models)
        write_json(prepared, inputs)
        result = process.run("predict", prepared, (manifest.parent, models))
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
