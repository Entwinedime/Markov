"""Evaluate one recorded prediction against a separate real target profile."""

import fcntl
from pathlib import Path

from markov.artifacts.request import positive_integer, read_json, reject_unknown, resolve_path
from markov.artifacts.writing import write_json
from markov.instrumentation.modeling import ModelingProcess


def evaluate(path: Path) -> dict:
    raw = read_json(path)
    reject_unknown(raw, {"prediction", "target", "manifest", "output", "memory_limit_gib"}, "target evaluation")
    directory = path.resolve().parent
    prediction = resolve_path(directory, raw["prediction"])
    target = resolve_path(directory, raw["manifest"])
    source = resolve_path(prediction.parent, read_json(prediction)["manifest"])
    output = resolve_path(directory, raw["output"])
    if (
        path.resolve().is_relative_to(output)
        or prediction.is_relative_to(output)
        or output.is_relative_to(prediction.parent)
    ):
        raise ValueError("Evaluation output must be separate from its request and prediction")
    if any(output.is_relative_to(manifest.parent) or manifest.is_relative_to(output) for manifest in (source, target)):
        raise ValueError("Evaluation output must be separate from base and target captures")

    output.mkdir(parents=True, exist_ok=True)
    with (output / "evaluation.lock").open("a") as lease:
        fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
        process = ModelingProcess(
            output / "evaluation",
            positive_integer(raw.get("memory_limit_gib", 4), "memory_limit_gib"),
            "markov_evaluate",
        )
        process.check_available()
        process.output.mkdir(parents=True, exist_ok=True)
        prepared = output / "evaluation_input.json"
        write_json(prepared, {"prediction": str(prediction), "target": raw["target"], "manifest": str(target)})
        result = process.run("evaluate", prepared, tuple(dict.fromkeys((prediction, source.parent, target.parent))))
        result["report"] = str(process.report)
        return result
