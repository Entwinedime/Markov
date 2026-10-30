"""Replay an observed DAG or a recorded generated path with explicit costs."""

import fcntl
from pathlib import Path

from markov.artifacts.request import ScalingRule, positive_integer, read_json, reject_unknown, resolve_path
from markov.artifacts.writing import write_json
from markov.instrumentation.modeling import ModelingProcess


def replay(path: Path) -> dict:
    raw = read_json(path)
    reject_unknown(
        raw,
        {"manifest", "prediction", "target", "node_scale", "overrides", "output", "memory_limit_gib", "inspect_name"},
        "static replay",
    )
    if ("manifest" in raw) == ("prediction" in raw):
        raise ValueError("Replay requires either manifest or prediction")
    directory = path.resolve().parent
    key = "manifest" if "manifest" in raw else "prediction"
    selected = resolve_path(directory, raw[key])
    output = resolve_path(directory, raw["output"])
    if selected.is_relative_to(output) or path.resolve().is_relative_to(output):
        raise ValueError("Static replay output must be separate from its input and request")
    if key == "manifest" and output.is_relative_to(selected.parent):
        raise ValueError("Static replay output must be outside its source capture")
    if (key == "prediction") != ("target" in raw):
        raise ValueError("Only predicted-path replay requires a target name")
    rules = [ScalingRule.parse(rule) for rule in raw.get("node_scale", [])]
    memory = positive_integer(raw.get("memory_limit_gib", 4), "memory_limit_gib")

    output.mkdir(parents=True, exist_ok=True)
    with (output / "replay.lock").open("a") as lease:
        fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
        process = ModelingProcess(output / "replay", memory)
        process.check_available()
        process.output.mkdir(parents=True, exist_ok=True)
        prepared = output / "replay_input.json"
        inputs = {
            key: str(selected),
            "node_scale": [{"name": rule.name, "factor": rule.factor} for rule in rules],
            "overrides": raw.get("overrides", []),
        }
        if "inspect_name" in raw:
            inputs["inspect_name"] = raw["inspect_name"]
        if "target" in raw:
            inputs["target"] = raw["target"]
        write_json(prepared, inputs)
        result = process.run("replay", prepared, (selected.parent if key == "manifest" else selected,))
        result["report"] = str(process.report)
        return result
