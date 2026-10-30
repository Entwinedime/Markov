"""Replay one source DAG with explicit cost hypotheses, without target prediction."""

import fcntl
from pathlib import Path

from markov.artifacts.request import ScalingRule, positive_integer, read_json, reject_unknown, resolve_path
from markov.artifacts.writing import write_json
from markov.instrumentation.modeling import ModelingProcess


def replay(path: Path) -> dict:
    raw = read_json(path)
    reject_unknown(
        raw, {"manifest", "node_scale", "overrides", "output", "memory_limit_gib", "inspect_name"}, "static replay"
    )
    directory = path.resolve().parent
    manifest = resolve_path(directory, raw["manifest"])
    output = resolve_path(directory, raw["output"])
    if output.is_relative_to(manifest.parent) or path.resolve().is_relative_to(output):
        raise ValueError("Static replay output must be separate from its source capture and request")
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
            "manifest": str(manifest),
            "node_scale": [{"name": rule.name, "factor": rule.factor} for rule in rules],
            "overrides": raw.get("overrides", []),
        }
        if "inspect_name" in raw:
            inputs["inspect_name"] = raw["inspect_name"]
        write_json(prepared, inputs)
        result = process.run("replay", prepared, (manifest.parent,))
        result["report"] = str(process.report)
        return result
