"""Import native idle scheduler turns separately from forward-region models."""

from pathlib import Path
from collections.abc import Iterator

from markov.adapters.sglang.region_measurements import context as context
from markov.adapters.sglang.region_measurements import events
from markov.adapters.sglang.region_measurements import metadata as metadata
from markov.artifacts.performance import Measurement


OPERATION = "scheduler.idle_turn"
IMPLEMENTATION = "sglang.scheduler.file_prefetch"
SCOPE = "caller_blocking_interval"


def recipes(captured: dict) -> dict:
    return {
        (OPERATION, IMPLEMENTATION, SCOPE): {
            "feature": None,
            "limitations": [
                "Native idle turns with a pending file prefetch and no active batch; same declared TP deployment.",
                "The response includes CPU broadcast, scheduler work and peer waits, not exclusive CPU service.",
                "Light observation overhead remains included; no busy serving or independent per-function model.",
            ],
        }
    }


def measurements(path: Path, captured: dict) -> Iterator[Measurement]:
    if captured["configuration"].get("hicache_storage_backend") != "file":
        raise ValueError("Scheduler idle sampling requires the native file backend")

    for position, event, warmup in events(path, captured):
        if event["name"] != "region.scheduler_idle":
            continue
        fields = event["args"]
        duration = fields["idle_interval_ns"]
        if type(duration) is not int or duration <= 0:
            raise ValueError("A measured scheduler idle turn must advance time")
        if fields["features"] != {"participants": captured["deployment"]["tensor_parallel"]}:
            raise ValueError("Scheduler idle measurement participants differ from the declared deployment")
        yield Measurement(
            position, "idle_interval_ns", OPERATION, IMPLEMENTATION, SCOPE, fields["features"], duration, warmup
        )
