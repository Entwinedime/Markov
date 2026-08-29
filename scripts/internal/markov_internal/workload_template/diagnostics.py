"""Evaluate read-only HiCache diagnostic snapshots for workload gates."""

from __future__ import annotations

from typing import Any, Iterable, Mapping

from .expand import RequestPlan, prefix_token_digest
from .schema import ConfigSpec, TemplateValidationError


def evaluate_assertion(
    snapshot: Mapping[str, Any],
    request: RequestPlan,
    range_kind: str,
    config: ConfigSpec,
) -> dict[str, Any]:
    """Project one canonical request range onto read-only per-rank node snapshots."""

    start, end = _assertion_range(request, range_kind, config)
    rank_results: list[dict[str, Any]] = []
    for rank in snapshot.get("ranks", []):
        if not isinstance(rank, dict):
            rank_results.append({"covered": False, "reason": "invalid_rank"})
            continue
        rank_results.append(_evaluate_range_on_rank(rank, request.prompt_token_ids, start, end, config.page_size))
    return {
        "range_start": start,
        "range_end": end,
        "range_tokens": end - start,
        "rank_results": rank_results,
        "storage": _aggregate_rank_property(rank_results, "storage"),
        "device": _aggregate_rank_property(rank_results, "device"),
        "host": _aggregate_rank_property(rank_results, "host"),
    }


def assertion_matches(observed: Mapping[str, Any], expected: Mapping[str, Any]) -> bool:
    """Compare one observation to all hard checkpoint predicates."""

    for property_name in ("storage", "device", "host"):
        required = expected.get(property_name)
        if required == "none_or_absent":
            if property_name != "device":
                return False
            if observed.get(property_name) == "none" or _range_is_strictly_absent(observed):
                continue
            return False
        if required != "ignore" and observed.get(property_name) != required:
            return False
    return True


def _assertion_range(request: RequestPlan, range_kind: str, config: ConfigSpec) -> tuple[int, int]:
    if range_kind == "anchor":
        return 0, request.anchor_tokens
    if range_kind == "admission_tail":
        admission_tail = config.page_size * ((request.tail_tokens - 1) // config.page_size)
        return request.anchor_tokens, request.anchor_tokens + admission_tail
    raise TemplateValidationError(f"unknown checkpoint range kind: {range_kind}")


def _evaluate_range_on_rank(
    rank: Mapping[str, Any],
    input_ids: Iterable[int],
    start: int,
    end: int,
    page_size: int,
) -> dict[str, Any]:
    if start < 0 or end <= start:
        return {"covered": False, "reason": "invalid_range"}
    token_ids = tuple(input_ids)
    if end > len(token_ids):
        return {"covered": False, "reason": "invalid_range"}
    expected_prefix_digests = {
        position: prefix_token_digest(token_ids[:position]) for position in range(0, len(token_ids) + 1, page_size)
    }
    nodes = rank.get("nodes")
    if not isinstance(nodes, list):
        return {"covered": False, "reason": "nodes_missing"}
    covered_nodes: list[Mapping[str, Any]] = []
    for node in nodes:
        if not isinstance(node, dict):
            continue
        node_start = node.get("prefix_start")
        node_end = node.get("prefix_end")
        if not isinstance(node_start, int) or not isinstance(node_end, int):
            continue
        if node_end <= start or node_start >= end:
            continue
        if expected_prefix_digests.get(node_start) != node.get("parent_token_digest"):
            continue
        if expected_prefix_digests.get(node_end) != node.get("path_token_digest"):
            continue
        covered_nodes.append(node)

    device_values: list[bool] = []
    host_values: list[bool] = []
    storage_values: list[bool] = []
    for page_start in range(start, end, page_size):
        page_end = page_start + page_size
        page_candidates = [
            node
            for node in covered_nodes
            if int(node["prefix_start"]) <= page_start and int(node["prefix_end"]) >= page_end
        ]
        if not page_candidates:
            return {"covered": False, "reason": "range_gap", "covered_until": page_start}
        node = min(
            page_candidates,
            key=lambda candidate: (
                bool(candidate.get("tombstone")),
                int(candidate["prefix_end"]) - int(candidate["prefix_start"]),
                int(candidate.get("node_id", -1)),
            ),
        )
        node_start = int(node["prefix_start"])
        if (page_start - node_start) % page_size != 0:
            return {"covered": False, "reason": "non_page_aligned_node_overlap"}
        local_start_page = (page_start - node_start) // page_size
        node_hashes = node.get("page_hashes")
        node_storage = node.get("storage_pages")
        if not isinstance(node_hashes, list) or not isinstance(node_storage, list):
            return {"covered": False, "reason": "page_hashes_missing"}
        if local_start_page >= len(node_hashes) or local_start_page >= len(node_storage):
            return {"covered": False, "reason": "page_hash_coverage_mismatch"}
        storage_values.append(bool(node_storage[local_start_page]))
        device_values.append(bool(node.get("device")))
        host_values.append(bool(node.get("host")))
    return {
        "covered": True,
        "storage": _residency_value(storage_values),
        "device": _residency_value(device_values),
        "host": _residency_value(host_values),
    }


def _residency_value(values: list[bool]) -> str:
    if not values or not any(values):
        return "none"
    return "all" if all(values) else "partial"


def _aggregate_rank_property(rank_results: Iterable[Mapping[str, Any]], property_name: str) -> str:
    materialized_results = list(rank_results)
    values = [result.get(property_name) for result in materialized_results if result.get("covered") is True]
    if not values or len(values) != len(materialized_results):
        return "uncovered"
    return str(values[0]) if len(set(values)) == 1 else "divergent"


def _range_is_strictly_absent(observed: Mapping[str, Any]) -> bool:
    start = observed.get("range_start")
    rank_results = observed.get("rank_results")
    return (
        isinstance(start, int)
        and isinstance(rank_results, list)
        and bool(rank_results)
        and all(
            isinstance(result, Mapping)
            and result.get("covered") is False
            and result.get("reason") == "range_gap"
            and result.get("covered_until") == start
            for result in rank_results
        )
    )
