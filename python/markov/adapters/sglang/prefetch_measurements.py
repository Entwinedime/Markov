"""Import closed prefetch control and background responses from independent runs."""

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
    result = {}
    features = {
        "issue": ["candidate_pages", "allocated_pages"],
        "check": ["operation_present", "state_reductions"],
        "accept": ["pages"],
    }
    for part in ("issue", "check", "accept"):
        if "prefetch." + part not in captured.get("measurement_ports", []):
            continue
        scopes = ("controller_enqueue_response", "controller_return_response") if part == "issue" else ("controller_group_response",)
        for scope in scopes:
            result[("cache.prefetch." + part, "sglang.HiRadixCache.prefetch", scope)] = {
                "feature": features[part],
                "curve": "page_tokens",
                "nonnegative": True,
                "extrapolation_features": features[part] if part != "check" else [],
                "limitations": [
                    "Group response after every rank enters; caller ownership is retained, not exclusive CPU service.",
                    "Query begins at enqueue and can overlap the issue return; final check precedes the stop snapshot and acceptance follows it.",
                    "Pending checks belong to idle-turn responses; warm serial small-tree data do not certify contention or large tree overhead.",
                ],
            }
    return result


def measurements(path: Path, captured: dict) -> Iterator[Measurement]:
    yield from (sample for sample in response_measurements(events(path, captured), captured) if sample.operation != OPERATION)


def response_measurements(records: Iterator[tuple[int, dict, bool]], captured: dict) -> Iterator[Measurement]:
    groups = defaultdict(list)
    for position, event, warmup in records:
        if event["name"] in ("prefetch.query", "prefetch.issue", "prefetch.check", "prefetch.accept"):
            groups[(event["name"], tuple(event["args"]["request_ids"]))].append((position, event, warmup))
    previous_end = 0
    for rows in sorted(groups.values(), key=lambda rows: max(Decimal(row[1]["ts"]) for row in rows)):
        ranks = {row[1]["args"]["participant"] for row in rows}
        if len(rows) != captured["deployment"]["tensor_parallel"] or ranks != set(range(len(rows))):
            raise ValueError("Prefetch response is missing a unique TP participant")
        end = max(int((Decimal(event["ts"]) + Decimal(event["dur"])) * 1000) for _, event, _ in rows)
        query = rows[0][1]["name"] == "prefetch.query"
        begin = (
            max(previous_end, max(event["args"]["queued_at_ns"] for _, event, _ in rows))
            if query
            else max(int(Decimal(event["ts"]) * 1000) for _, event, _ in rows)
        )
        duration = end - begin
        if duration < 0:
            raise ValueError("Prefetch response precedes its eligible group start")
        if query:
            previous_end = end
        features = {
            key: max(event["args"]["features"][key] for _, event, _ in rows) for key in rows[0][1]["args"]["features"]
        }
        if rows[0][1]["name"] == "prefetch.issue":
            queued = end
            if features["allocated_pages"]:
                queries = groups[("prefetch.query", tuple(rows[0][1]["args"]["request_ids"]))]
                if {row[1]["args"]["participant"] for row in queries} != ranks or len(queries) != len(rows):
                    raise ValueError("Prefetch issue needs one observed enqueue port per participant")
                queued = max(event["args"]["queued_at_ns"] for _, event, _ in queries)
            if not begin <= queued <= end:
                raise ValueError("Prefetch enqueue lies outside its issuing group response")
            spans = [("enqueue", begin, queued)]
            if features["allocated_pages"]:
                spans.append(("return", queued, end))
            for port, start, finish in spans:
                yield Measurement(
                    rows[0][0],
                    f"controller_{port}_response_ns",
                    "cache.prefetch.issue",
                    "sglang.HiRadixCache.prefetch",
                    f"controller_{port}_response",
                    features,
                    finish - start,
                    all(row[2] for row in rows),
                )
            continue
        yield Measurement(
            rows[0][0],
            "query_group_response_ns" if query else "controller_group_response_ns",
            OPERATION if query else "cache." + rows[0][1]["name"],
            IMPLEMENTATION if query else "sglang.HiRadixCache.prefetch",
            SCOPE if query else "controller_group_response",
            features,
            duration,
            all(row[2] for row in rows),
        )
