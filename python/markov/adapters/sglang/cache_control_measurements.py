"""Import failed native load controls, excluding delegated backup work."""

from collections.abc import Iterator
from pathlib import Path

from markov.adapters.sglang.region_measurements import context as context
from markov.adapters.sglang.region_measurements import events
from markov.adapters.sglang.region_measurements import metadata as metadata
from markov.artifacts.performance import Measurement


OPERATION = "cache.load.failed"
IMPLEMENTATION = "sglang.HiRadixCache.init_load_back"
SCOPE = "thread_cpu"


def recipes(captured: dict) -> dict:
    return {
        (OPERATION, IMPLEMENTATION, SCOPE): {
            "feature": ["pages", "segments"],
            "curve": "page_tokens",
            "nonnegative": True,
            "limitations": [
                "Native init_load_back after two failed device allocations; threshold/quota skips are excluded.",
                "Thread CPU excludes successful nested backups and their completion checks, which are delegated work.",
                "Small serial synthetic reuse workload; unrelated tree size, logging and light observation add uncertainty.",
            ],
        }
    }


def measurements(path: Path, captured: dict) -> Iterator[Measurement]:
    for position, event, warmup in events(path, captured):
        if event["name"] != "region.load_failed":
            continue
        fields = event["args"]
        duration = fields["thread_cpu_ns"] - fields["delegated_cpu_ns"]
        if type(duration) is not int or duration < 0:
            raise ValueError(f"Delegated backup CPU exceeds failed load CPU: {path}, record {position}")
        yield Measurement(
            position, "exclusive_thread_cpu_ns", OPERATION, IMPLEMENTATION, SCOPE, fields["features"], duration, warmup
        )
