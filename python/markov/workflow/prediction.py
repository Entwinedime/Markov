"""Run one prepared prediction session, with base and costs isolated from evaluator data."""

from dataclasses import asdict
import os
from pathlib import Path
import subprocess

from markov.artifacts.request import PredictionRequest, read_json
from markov.artifacts.workload import Workload
from markov.artifacts.writing import write_json
from markov.workflow.capture import InferenceCapture


def predict(request: PredictionRequest) -> dict:
    if request.models is None:
        raise ValueError("Prediction requires selected independent performance.models; use maintain to prepare them")
    if request.execution.kernel_cache_initial_state is None:
        raise ValueError("Prediction requires execution.kernel_cache_initial_state: empty or prepared")
    request.models.stat()
    workload = Workload.load(request.workload)
    base = InferenceCapture(request).run()
    if base["status"] not in ("completed", "reused"):
        return base

    manifest = Path(base["manifest"])
    output = request.execution.output / "prediction"
    if output.is_relative_to(manifest.parent) or request.models.is_relative_to(output):
        raise ValueError("Prediction output must be separate from base capture and independent model inputs")
    output.mkdir(parents=True, exist_ok=True)
    prepared = request.execution.output / "prediction_input.json"
    write_json(
        prepared,
        {
            "manifest": str(manifest),
            "models": str(request.models),
            "deployment": asdict(request.deployment),
            "workload": asdict(workload),
            "targets": [asdict(target) for target in request.targets],
            "kernel_cache_initial_state": request.execution.kernel_cache_initial_state,
        },
    )
    executable = Path(__file__).resolve().parents[3] / "build/modeling/Release/markov_core"
    cid = output / "container.id"
    if cid.exists():
        current = subprocess.run(
            ["docker", "inspect", "--format", "{{.State.Running}}", cid.read_text().strip()],
            capture_output=True,
            text=True,
        )
        if current.returncode == 0 and current.stdout.strip() == "true":
            raise ValueError("This prediction output still has a running container")
    cid.unlink(missing_ok=True)
    report = output / "report.json"
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
        str(cid),
        "--memory",
        f"{request.execution.memory_limit_gib or 4}g",
        "--memory-swap",
        f"{request.execution.memory_limit_gib or 4}g",
    ]
    # No repository/data-root mount: the evaluator's normal observations and
    # target traces cannot enter this container, even as unused neighbors.
    for source, target, writable in (
        (executable, "/opt/markov/markov_core", False),
        (manifest.parent, str(manifest.parent), False),
        (request.models, str(request.models), False),
        (prepared, str(prepared), False),
        (output, str(output), True),
    ):
        args += ["--mount", f"type=bind,source={source},target={target}" + ("" if writable else ",readonly")]
    args += [
        os.environ.get("MARKOV_MODELING_IMAGE", "markov-modeling:ubuntu24.04"),
        "/opt/markov/markov_core",
        "predict",
        str(prepared),
        str(report),
    ]
    write_json(output / "invocation.json", args)
    with (output / "run.log").open("w", encoding="utf-8") as log:
        process = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT)
        try:
            code = process.wait()
        finally:
            if process.poll() is None:
                if cid.exists():
                    subprocess.run(["docker", "rm", "-f", cid.read_text().strip()], stdout=log, stderr=log, timeout=20)
                process.terminate()
                process.wait(timeout=20)
    if code:
        return {"status": "failed", "exit_code": code, "log": str(output / "run.log")}
    result = read_json(report)
    result["report"] = str(report)
    return result
