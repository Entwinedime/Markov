"""Strict operation-level matching for diagnostic HiCache oracle-cost replay."""

from __future__ import annotations

from collections import defaultdict
from typing import Any

from ....evaluation.scoring import observed_direct_cost
from ....io_model_contract import io_observation_ready


SHAPE_FIELDS = ("operation_count", "page_count", "byte_count")


class OracleCostMatchError(ValueError):
    """Raised when exact shapes or a unique trace-local lane binding are unavailable."""


def _logical_inputs(run: dict[str, Any]) -> dict[str, int]:
    """Resolve process IDs within one trace, never compare PIDs across runs."""

    inputs: dict[str, int] = {}
    for row in run["source_phase_observations"]["observations"]:
        if inputs.setdefault(row["pid"], row["logical_input"]) != row["logical_input"]:
            raise OracleCostMatchError("one process belongs to multiple logical inputs")
    return inputs


def target_operation_cell(ledger: dict, run: dict, kv_bytes_per_token: int) -> dict:
    """Project actual operation shapes and current control clocks; no predicted costs."""

    inputs = _logical_inputs(run)
    by_kind: dict[str, dict] = {}
    for row in run["source_io_observations"]["observations"]:
        tokens, direction = row["completed_tokens"], row["direction"]
        pages = row["service_page_count"]
        if pages == 0 and tokens == 0 and direction != "storage_to_host":
            continue  # Inferred zero Load and other no-ops have no owned cost.
        observed = io_observation_ready(row) and (pages == 0 or row["service_observed"])
        if direction == "storage_to_host":
            observed = observed and row["terminal_control_observed"]
        elif direction == "host_to_device":
            observed = observed and row["admission_control_observed"]
        if not observed or row["page_size"] <= 0 or tokens % row["page_size"]:
            raise OracleCostMatchError(f"target operation shape/cost not observed: {row['record_id']}")
        if "source_start_us" not in row or "timing_fact_node_id" not in row:
            raise OracleCostMatchError("target observation lacks operation order; re-extract into a fresh score output")
        service, control = observed_direct_cost(row)
        record = {"record_id": row["record_id"], "resource_lane": f"{row['resource_scope']}/{direction}",
                  "logical_input": inputs[row["pid"]], "source_start_us": row["source_start_us"],
                  "timing_fact_node_id": row["timing_fact_node_id"],
                  "operation_count": row["operation_count"], "page_count": pages,
                  "byte_count": pages * row["page_size"] * kv_bytes_per_token,
                  "completed_page_count": tokens // row["page_size"],
                  "service_us": service, "control_us": control}
        by_kind.setdefault(row["kind"], {"records": []})["records"].append(record)
    return {"status": "READY", "config_id": ledger["target_config_id"], "run_id": ledger["target_run_id"],
            "workload_id": ledger["workload_id"], "by_kind": by_kind}


def build_score_only_target_oracle_catalog(
    pair_ledger: dict[str, Any],
    observed_cell: dict[str, Any],
    source_run: dict[str, Any],
) -> dict[str, Any]:
    """Bind one predicted structure to a score-only operation ledger.

    The single-base experiment deliberately creates no target-self DAG models.
    Its already exact target structure therefore supplies the effect identities,
    while separate target score data supplies only observed direct
    costs.  Target E2E and target parameters are never consumed.
    """

    _require_ready(pair_ledger)
    if pair_ledger.get("is_self") is True:
        raise OracleCostMatchError("score-only oracle catalog requires a cross pair")
    if observed_cell.get("status") != "READY":
        raise OracleCostMatchError("score-only target operation ledger is not READY")
    for field, pair_field in (
        ("config_id", "target_config_id"),
        ("run_id", "target_run_id"),
        ("workload_id", "workload_id"),
    ):
        if str(observed_cell.get(field) or "") != str(pair_ledger.get(pair_field) or ""):
            raise OracleCostMatchError(f"score-only target {field} does not match the predicted pair")
    observed_payload = {
        "status": "READY",
        "by_kind": observed_cell.get("by_kind"),
    }
    bridge = {
        "target_actual": observed_payload,
        "target_predicted": pair_ledger.get("target_predicted"),
    }
    actual = _flatten_records(bridge, "target_actual")
    predicted = _flatten_records(bridge, "target_predicted")
    return _build_target_oracle_catalog(
        actual,
        predicted,
        source_run=source_run,
        source_scope_records=pair_ledger["source_scope_records"],
        workload_id=pair_ledger.get("workload_id"),
        target_run_id=pair_ledger.get("target_run_id"),
    )


def _build_target_oracle_catalog(
    actual: list[dict[str, Any]],
    predicted: list[dict[str, Any]],
    *,
    source_run: dict[str, Any],
    source_scope_records: list[dict],
    workload_id: Any,
    target_run_id: Any,
) -> dict[str, Any]:
    scope_to_lane = _map_scopes_to_observed_lanes(actual, predicted, source_run, source_scope_records)

    matched: list[dict[str, Any]] = []
    consumed_record_ids: set[str] = set()
    kinds = sorted({str(row["kind"]) for row in actual} | {str(row["kind"]) for row in predicted})
    for kind in kinds:
        scopes = sorted({str(row["resource_scope"]) for row in predicted if row["kind"] == kind})
        for scope in scopes:
            lane = scope_to_lane.get(scope)
            if lane is None:
                raise OracleCostMatchError(f"no observed resource lane for scope {scope!r}")
            predicted_group = sorted(
                (row for row in predicted if row["kind"] == kind and row["resource_scope"] == scope),
                key=_predicted_order,
            )
            actual_group = sorted(
                (row for row in actual if row["kind"] == kind and row["lane_base"] == lane),
                key=_actual_order,
            )
            group_matches = _partition_observed_records_by_shape(
                predicted_group,
                actual_group,
                kind,
                scope,
            )
            for model_record, observed_records in group_matches:
                record_ids = [str(row["record_id"]) for row in observed_records]
                duplicates = consumed_record_ids.intersection(record_ids)
                if duplicates:
                    raise OracleCostMatchError(f"observed records matched more than once: {sorted(duplicates)}")
                consumed_record_ids.update(record_ids)
                matched.append(_oracle_cost_record(model_record, observed_records))

    all_record_ids = {str(row["record_id"]) for row in actual}
    if consumed_record_ids != all_record_ids:
        missing = sorted(all_record_ids - consumed_record_ids)
        raise OracleCostMatchError(f"unmatched target-observed records: {missing[:5]}")
    effect_ids = [str(row["effect_id"]) for row in matched]
    if len(effect_ids) != len(set(effect_ids)):
        raise OracleCostMatchError("target self-pair contains duplicate predicted effect IDs")
    if len(matched) != len(predicted):
        raise OracleCostMatchError(f"matched effect count {len(matched)} != predicted effect count {len(predicted)}")

    return {
        "workload_id": workload_id,
        "target_run_id": target_run_id,
        "costs": sorted(matched, key=lambda row: (int(row["logical_order_epoch"]), str(row["effect_id"]))),
    }


def build_pair_oracle_override(pair_ledger: dict[str, Any], target_catalog: dict[str, Any]) -> dict[str, Any]:
    """Attach target-observed costs to one source-derived target structure."""

    _require_ready(pair_ledger)
    if pair_ledger.get("workload_id") != target_catalog.get("workload_id"):
        raise OracleCostMatchError("pair and target catalog workload identities differ")
    if pair_ledger.get("target_run_id") != target_catalog.get("target_run_id"):
        raise OracleCostMatchError("pair and target catalog run identities differ")
    catalog_by_effect = {str(row["effect_id"]): row for row in target_catalog.get("costs", [])}
    predicted = _flatten_records(pair_ledger, "target_predicted")
    predicted_by_effect = {str(row["effect_id"]): row for row in predicted}
    if len(predicted_by_effect) != len(predicted):
        raise OracleCostMatchError("pair contains duplicate predicted effect IDs")
    if set(predicted_by_effect) != set(catalog_by_effect):
        missing = sorted(set(catalog_by_effect) - set(predicted_by_effect))
        extra = sorted(set(predicted_by_effect) - set(catalog_by_effect))
        raise OracleCostMatchError(f"pair/target effect identity mismatch: missing={missing[:3]}, extra={extra[:3]}")

    costs: list[dict[str, Any]] = []
    for effect_id, structure in predicted_by_effect.items():
        oracle = catalog_by_effect[effect_id]
        _require_same_target_shape(structure, oracle)
        costs.append(
            {
                "effect_id": effect_id,
                "effect_type": structure["effect_type"],
                "direction": structure["direction"],
                "resource_scope": structure["resource_scope"],
                "resource_lane": structure["resource_lane"],
                "logical_order_epoch": int(structure["logical_order_epoch"]),
                "operation_count": int(structure["operation_count"]),
                "page_count": int(structure["page_count"]),
                "byte_count": int(structure["byte_count"]),
                "service_us": int(oracle["service_us"]),
                "control_us": int(oracle["control_us"]),
            }
        )
    return {"costs": sorted(costs, key=lambda row: (int(row["logical_order_epoch"]), str(row["effect_id"])))}


def _require_ready(ledger: dict[str, Any]) -> None:
    if ledger.get("status") != "READY" or ledger.get("errors"):
        raise OracleCostMatchError(f"direct-effect ledger is not READY: {ledger.get('pair_id')}")


def _flatten_records(ledger: dict[str, Any], side: str) -> list[dict[str, Any]]:
    payload = ledger.get(side)
    if not isinstance(payload, dict) or payload.get("status") != "READY":
        raise OracleCostMatchError(f"{side} is not READY")
    rows: list[dict[str, Any]] = []
    by_kind = payload.get("by_kind")
    if not isinstance(by_kind, dict):
        raise OracleCostMatchError(f"{side}.by_kind is missing")
    for kind, aggregate in by_kind.items():
        if not isinstance(aggregate, dict):
            continue
        for raw in aggregate.get("records", []):
            row = dict(raw)
            row["kind"] = str(kind)
            if side == "target_actual":
                lane = str(row.get("resource_lane") or "")
                if "/" not in lane:
                    raise OracleCostMatchError(f"observed resource lane has no direction suffix: {lane!r}")
                row["lane_base"] = lane.rsplit("/", 1)[0]
            rows.append(row)
    return rows


def _map_scopes_to_observed_lanes(
    actual: list[dict[str, Any]], predicted: list[dict[str, Any]], source_run: dict[str, Any], source_scope_records: list[dict]
) -> dict[str, str]:
    # Source operation IDs bind the predicted scope to its own trace's rank.
    # Target node IDs, process IDs and arrival order are not cross-trace identities.
    inputs = _logical_inputs(source_run)
    source = {row["record_id"]: row for row in source_run["source_io_observations"]["observations"]}
    scopes: dict[str, set[int]] = {row["resource_scope"]: set() for row in predicted}
    lanes: dict[int, set[str]] = defaultdict(set)
    for row in actual:
        lanes[row["logical_input"]].add(row["lane_base"])
    for row in source_scope_records:
        if row["resource_scope"] not in scopes:
            continue
        ranks = scopes[row["resource_scope"]]
        for record_id in row["source_io_operation_record_ids"]:
            ranks.add(inputs[source[record_id]["pid"]])
    mapping = {}
    for scope, ranks in scopes.items():
        if len(ranks) != 1 or len(lanes[next(iter(ranks))]) != 1:
            raise OracleCostMatchError(f"no unique logical-input lane for predicted scope {scope}: {sorted(ranks)}")
        mapping[scope] = next(iter(lanes[next(iter(ranks))]))
    if len(set(mapping.values())) != len(mapping) or set(mapping.values()) != {row["lane_base"] for row in actual}:
        raise OracleCostMatchError("predicted scopes and observed logical-input lanes are not one-to-one")
    return mapping


def _partition_observed_records_by_shape(
    predicted: list[dict[str, Any]],
    actual: list[dict[str, Any]],
    kind: str,
    scope: str,
) -> list[tuple[dict[str, Any], list[dict[str, Any]]]]:
    """Match exact shapes independent of order, then exact observed subsets.

    Target runtime batching may merge adjacent observed operations that the
    predicted target structure represents as one effect, and async execution may
    reorder otherwise identical operations.  This matcher never invents or
    proportionally splits a cost: every predicted shape receives one or more
    whole observed records whose operation/page/byte vector sums exactly.
    """

    remaining = list(sorted(actual, key=_actual_order))
    matches: list[tuple[dict[str, Any], list[dict[str, Any]]]] = []
    for model_record in sorted(predicted, key=_predicted_order):
        goal = _shape(model_record)
        exact_index = next(
            (index for index, row in enumerate(remaining) if _shape(row) == goal),
            None,
        )
        if exact_index is not None:
            matches.append((model_record, [remaining.pop(exact_index)]))
            continue
        subset = _exact_shape_subset(remaining, goal)
        if not subset:
            raise OracleCostMatchError(
                f"shape-multiset operation volume mismatch for {kind}/{scope}: "
                f"predicted={goal}, effect={model_record.get('effect_id')}"
            )
        observed = [remaining[index] for index in subset]
        for index in reversed(subset):
            remaining.pop(index)
        matches.append((model_record, observed))
    if remaining:
        residual = tuple(sum(_shape(row)[axis] for row in remaining) for axis in range(len(SHAPE_FIELDS)))
        raise OracleCostMatchError(f"unused target-observed shape volume for {kind}/{scope}: {residual}")
    return matches


def _exact_shape_subset(
    rows: list[dict[str, Any]],
    goal: tuple[int, ...],
) -> list[int]:
    candidates = [
        (index, _shape(row))
        for index, row in enumerate(rows)
        if all(value <= limit for value, limit in zip(_shape(row), goal))
    ]
    states: dict[tuple[int, ...], list[int]] = {(0, 0, 0): []}
    for index, shape in candidates:
        for total, selected in list(states.items())[::-1]:
            combined = tuple(left + right for left, right in zip(total, shape))
            if any(value > limit for value, limit in zip(combined, goal)):
                continue
            states.setdefault(combined, [*selected, index])
        if goal in states:
            return states[goal]
    return []


def _shape(row: dict[str, Any]) -> tuple[int, int, int]:
    return tuple(int(row.get(field) or 0) for field in SHAPE_FIELDS)


def _oracle_cost_record(model_record: dict[str, Any], observed: list[dict[str, Any]]) -> dict[str, Any]:
    effect_type = str(model_record["effect_type"])
    return {
        "effect_id": str(model_record["effect_id"]),
        "effect_type": effect_type,
        "direction": str(model_record["direction"]),
        "resource_scope": str(model_record["resource_scope"]),
        "resource_lane": str(model_record["resource_lane"]),
        "logical_order_epoch": int(model_record["logical_order_epoch"]),
        **{field: int(model_record[field]) for field in SHAPE_FIELDS},
        "service_us": sum(int(row.get("service_us") or 0) for row in observed),
        "control_us": sum(int(row.get("control_us") or 0) for row in observed),
    }


def _require_same_target_shape(structure: dict[str, Any], oracle: dict[str, Any]) -> None:
    fields = ("effect_type", "direction", "resource_scope", "resource_lane", *SHAPE_FIELDS)
    mismatches = [field for field in fields if structure.get(field) != oracle.get(field)]
    if mismatches:
        raise OracleCostMatchError(
            f"target effect shape changed across sources for {structure.get('effect_id')}: {mismatches}"
        )


def _predicted_order(row: dict[str, Any]) -> tuple[int, str]:
    return int(row.get("logical_order_epoch") or 0), str(row.get("effect_id") or "")


def _actual_order(row: dict[str, Any]) -> tuple[int, int, str]:
    return (
        int(row.get("source_start_us") or 0),
        int(row.get("timing_fact_node_id") or 0),
        str(row.get("record_id") or ""),
    )
