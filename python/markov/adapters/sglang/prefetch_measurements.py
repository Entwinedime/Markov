"""Import collective background query responses; no target performance is used."""

from collections import defaultdict
from collections.abc import Iterator
from decimal import Decimal
from pathlib import Path

from markov.adapters.sglang.region_measurements import context as context
from markov.adapters.sglang.region_measurements import events
from markov.adapters.sglang.region_measurements import metadata as metadata
from markov.artifacts.performance import Measurement


OPERATION = "cache.prefetch.query"
IMPLEMENTATION = "sglang.file.prefetch_query"
SCOPE = "query_group_response"


def recipes(captured: dict) -> dict:
    return {
        (OPERATION, IMPLEMENTATION, SCOPE): {
            "feature": ["hashed_pages", "checked_pages"],
            "curve": "page_tokens",
            "nonnegative": True,
            "extrapolation_features": ["hashed_pages", "checked_pages"],
            "limitations": [
                "Closed query-group response from eligible enqueue to completed native rank reduction, excluding prior query backlog.",
                "Warm local file metadata and serial requests; thread wakeup and light observation remain included.",
                "One shared query lane approximates small rank-return skew; concurrent file changes during a query are not certified.",
            ],
        }
    }


def measurements(path: Path, captured: dict) -> Iterator[Measurement]:
    groups = defaultdict(list)
    for position, event, warmup in events(path, captured):
        if event["name"] == "prefetch.query":
            groups[tuple(event["args"]["request_ids"])].append((position, event, warmup))
    previous_end = 0
    for rows in sorted(groups.values(), key=lambda rows: max(Decimal(row[1]["ts"]) for row in rows)):
        ranks = {row[1]["args"]["participant"] for row in rows}
        if len(rows) != captured["deployment"]["tensor_parallel"] or ranks != set(range(len(rows))):
            raise ValueError(f"Background query is missing a unique TP participant: {path}")
        end = max(int((Decimal(event["ts"]) + Decimal(event["dur"])) * 1000) for _, event, _ in rows)
        begin = max(previous_end, max(event["args"]["queued_at_ns"] for _, event, _ in rows))
        duration = end - begin
        if duration < 0:
            raise ValueError(f"Background query response precedes its eligible enqueue: {path}")
        previous_end = end
        features = {
            key: max(event["args"]["features"][key] for _, event, _ in rows) for key in rows[0][1]["args"]["features"]
        }
        yield Measurement(
            rows[0][0],
            "query_group_response_ns",
            OPERATION,
            IMPLEMENTATION,
            SCOPE,
            features,
            duration,
            all(row[2] for row in rows),
        )
