"""Score-only semantic work comparison; never supplies costs to prediction."""

from __future__ import annotations

from collections import Counter
import json
from pathlib import Path
from typing import Any

from markov_internal.common.io import load_json, write_json
from markov_internal.modeling.backend import append_option, execute_trace_graph, trace_graph_executable
from markov_internal.modeling.source_dag.profiles import ProfileRunRef
from markov_internal.applications.accuracy.shape.shape_oracle import (
    extract_target_shape_oracle,
    patch_probe_contract_enabled,
)
from markov_internal.applications.accuracy.precedence import compare_precedence


def extract_target_structure(target: ProfileRunRef, output: Path, *, threads: int, file_threads: int) -> dict[str, Any]:
    """Extract selected target work using current code, without a model config.

    The source-only backend reads this evaluator's target manifest. It does not
    execute a target predictor, inject oracle costs or reuse old extraction files.
    Requesting a structure report skips independent cost-sample export; cost
    attribution readiness is not a prerequisite for observing target structure.
    """

    output.mkdir(parents=True, exist_ok=True)
    summary = output / "observations.json"
    operator_structure = output / "phase_structure.json"
    command = [
        str(trace_graph_executable()),
        "--profile-manifest",
        str(target.manifest_path),
        "--run-summary",
        str(summary),
        "--source-observations-only",
        "--trace-channels",
        "torch,ld_preload,python_probe",
        "--structure-report",
        str(operator_structure),
    ]
    append_option(command, "--threads", threads)
    append_option(command, "--file-threads", file_threads)
    window = target.workload_window
    if window is not None:
        append_option(command, "--trace-window-start-us", window.start_ns // 1000)
        append_option(command, "--trace-window-end-us", window.end_ns // 1000)
    execute_trace_graph(command)

    operators = load_json(operator_structure)
    # Both observations must include the same declared preparation prefix.
    # Its observed start is not a predicted time or an extension of HTTP E2E.
    shape = extract_target_shape_oracle(
        target.python_probe_files,
        patch_probe_contract_ready=patch_probe_contract_enabled(load_json(target.manifest_path)),
        window_start_us=operators["request_context"]["observed_request_start_us"],
        window_end_us=window.end_ns // 1000 if window else None,
    )
    result = {
        "effects": shape,
        "phases": load_json(summary)["source_phase_observations"],
        "phase_operators": operators,
        "source_manifest": str(target.manifest_path),
    }
    write_json(output / "structure_observation.json", result)
    return result


def _correspondence(predicted: list[dict[str, Any]], actual: list[dict[str, Any]], identity: str) -> dict[str, Any]:
    expected_counts = Counter(row[identity] for row in predicted)
    actual_counts = Counter(row[identity] for row in actual)
    ambiguous = {
        key
        for key in expected_counts.keys() | actual_counts.keys()
        if expected_counts[key] > 1 or actual_counts[key] > 1
    }
    invalid = {
        key
        for key in expected_counts.keys() | actual_counts.keys()
        if (key < 0 if identity == "logical_input" else not key)
    }
    ambiguous.update(invalid)
    expected = {row[identity]: row for row in predicted if row[identity] not in ambiguous}
    observed = {row[identity]: row for row in actual if row[identity] not in ambiguous}
    common = expected.keys() & observed.keys()
    return {
        "predicted": expected,
        "actual": observed,
        "common": sorted(common),
        "unmatched_prediction": sorted(expected.keys() - observed.keys()),
        "unmatched_target": sorted(observed.keys() - expected.keys()),
        "ambiguous": sorted(ambiguous),
        "invalid_identity": sorted(invalid),
    }


def compare_structure(predicted: dict[str, Any], target: dict[str, Any], *, details: bool = False) -> dict[str, Any]:
    """Keep correspondence, work differences and dependency coverage separate.

    Compare effect work, serial request shapes and owned phase operators/edges.
    Phase precedence includes paths through unowned intermediates. Their own
    identity/resource ordering and HiCache control remain unproven, so matching
    fields cannot prove whole-graph equivalence.
    """

    oracle = target["effects"]
    correspondence = _correspondence(predicted["effects"], oracle["effects"], "effect_key")
    work_fields = (
        "direction",
        "operation_count",
        "effective_page_count",
        "completed_page_count",
        "storage_existing_page_count",
        "storage_new_page_count",
        "storage_batches",
    )
    differences = []
    unobservable = []
    for key in correspondence["common"]:
        expected, actual = correspondence["predicted"][key], correspondence["actual"][key]
        if actual["actual_state"] in {"unobservable", "ambiguous"}:
            unobservable.append({"effect_key": key, "reason": actual["blocker"]})
            continue
        fields = [field for field in work_fields if expected[field] != actual[field]]
        if expected["target_effect_state"] != actual["actual_state"]:
            fields.append("effect_state")
        if fields:
            differences.append(
                {"effect_key": key, "fields": fields, "schedule_sensitivity": expected["schedule_sensitivity"]}
            )

    target_prefills, target_decodes = [], []
    unsupported_batches = 0
    for row in target["phases"]["observations"]:
        if len(row["request_ids"]) != 1:
            unsupported_batches += 1
            continue
        target_prefills.append(
            {
                "logical_input": row["logical_input"],
                "request_id": row["request_ids"][0],
                "batch_size": row["batch_size"],
                "page_size_tokens": row["source_page_size"],
                "prompt_token_count": row["prompt_token_count"],
                "prefill_token_count": row["prefill_token_count"],
                "reusable_prefix_token_count": row["prompt_token_count"] - row["prefill_token_count"],
            }
        )
        target_decodes.append(
            {
                "logical_input": row["logical_input"],
                "request_id": row["request_ids"][0],
                "page_size_tokens": row["source_page_size"],
                "prompt_token_count": row["prompt_token_count"],
                "iteration_count": row["decode_iteration_count"],
            }
        )

    def request_rows(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
        # logical_input identifies a rank, not a request. Match the workload's
        # explicit request identity within that rank; never pair by position.
        return [
            dict(
                row,
                request_identity=(
                    f"{row['logical_input']}|{row['request_id']}"
                    if row["logical_input"] >= 0 and row["request_id"]
                    else ""
                ),
            )
            for row in rows
        ]

    phases = {}
    for kind, observed in (("prefills", target_prefills), ("decodes", target_decodes)):
        expected = request_rows(predicted[kind])
        actual = request_rows(observed)
        matched = _correspondence(expected, actual, "request_identity")
        changed = [
            {
                "request_identity": key,
                "fields": [
                    field
                    for field in matched["actual"][key]
                    if field != "request_identity" and matched["predicted"][key][field] != matched["actual"][key][field]
                ],
            }
            for key in matched["common"]
        ]
        phases[kind] = {
            "matched": len(matched["common"]),
            "unmatched_prediction": matched["unmatched_prediction"],
            "unmatched_target": matched["unmatched_target"],
            "ambiguous": matched["ambiguous"],
            "invalid_identity": matched["invalid_identity"],
            "differences": [row for row in changed if row["fields"]],
        }

    effect_work_exact = (
        bool(correspondence["common"])
        and oracle["ready"]
        and predicted["effect_status"] == "ready"
        and not any(
            (
                correspondence["unmatched_prediction"],
                correspondence["unmatched_target"],
                correspondence["ambiguous"],
                differences,
                unobservable,
            )
        )
    )
    phase_work_exact = (
        target["phases"]["status"] == "ready" and predicted["phase_status"] == "ready" and not unsupported_batches
    )
    phase_work_exact = phase_work_exact and all(
        row["matched"]
        and not any((row["unmatched_prediction"], row["unmatched_target"], row["ambiguous"], row["differences"]))
        for row in phases.values()
    )
    generated_calls = {row["call_identity"] for row in predicted["executed_phase_tensor_operations"]["operators"]}
    operator_comparison = _compare_phase_operators(
        predicted["phase_operators"],
        target["phase_operators"],
        generated_calls,
        predicted["collective_operations"],
        target["phase_operators"]["collective_operations"],
        details=details,
    )
    report = {
        "status": "PARTIAL_COVERAGE",
        "scope": predicted["scope"],
        "full_structure_exact": False,
        "oracle_cost_used": False,
        "diagnostic_details_retained": details,
        "effects": {
            "predicted_count": len(predicted["effects"]),
            "target_count": len(oracle["effects"]),
            "matched_count": len(correspondence["common"]),
            "target_observation_blockers": oracle.get("observation_blockers"),
            "unmatched_prediction_states": dict(
                Counter(
                    correspondence["predicted"][key]["target_effect_state"]
                    for key in correspondence["unmatched_prediction"]
                )
            ),
            "unmatched_target_states": dict(
                Counter(correspondence["actual"][key]["actual_state"] for key in correspondence["unmatched_target"])
            ),
            "count_interpretation": "Effects include not-required decisions, not just executed I/O; unmatched decisions do not by themselves prove extra device work.",
            "work_exact_within_scope": effect_work_exact,
            "unmatched_prediction": correspondence["unmatched_prediction"],
            "unmatched_target": correspondence["unmatched_target"],
            "ambiguous": correspondence["ambiguous"],
            "invalid_identity": correspondence["invalid_identity"],
            "unobservable": unobservable,
            "differences": differences,
        },
        "phases": {
            "work_exact_within_scope": phase_work_exact,
            "unsupported_target_batches": unsupported_batches,
            **phases,
        },
        "phase_operators": operator_comparison,
        "collective_operations": _compare_collective_operations(
            predicted["collective_operations"], target["phase_operators"]["collective_operations"], details=details
        ),
        "allocator_work": _compare_allocator_work(
            predicted["allocator_work"], target["phase_operators"]["allocator_work"]
        ),
        "allocator_tensor_operations": _compare_tensor_operations(
            predicted["executed_allocator_operations"],
            target["phase_operators"]["allocator_tensor_operations"],
            details=details,
        ),
        "generated_work_inventory": predicted["generated_work_inventory"],
        "phase_tensor_operations": _compare_tensor_operations(
            predicted["executed_phase_tensor_operations"],
            target["phase_operators"]["phase_tensor_operations"],
            details=details,
        ),
        "dependencies": operator_comparison["dependencies"],
        "uncompared": predicted["uncompared"],
    }
    if not details:
        _compact_comparison(report)
    return report


def _compare_collective_operations(
    predicted: dict[str, Any], target: dict[str, Any], *, details: bool
) -> dict[str, Any]:
    """Compare logical all-reduce work, not native kernel names or durations.

    Request/round/layer/callsite and the participant roster establish identity.
    Completion precedence comes from each actual graph, not the target work
    plan. Matching here does not prove event or packet-level protocol fidelity.
    """

    matched = _correspondence(predicted["operators"], target["operators"], "operator_identity")
    differences = []
    for identity in matched["common"]:
        expected, observed = matched["predicted"][identity], matched["actual"][identity]
        fields = [field for field in ("implementation", "features") if expected[field] != observed[field]]
        if fields:
            differences.append({"operator_identity": identity, "fields": fields})

    common = set(matched["common"])
    return {
        "matched": len(common),
        "differences": differences,
        **{
            field: matched[field]
            for field in ("unmatched_prediction", "unmatched_target", "ambiguous", "invalid_identity")
        },
        "prediction_issues": predicted["issues"],
        "target_issues": target["issues"],
        "work_exact_within_scope": bool(common)
        and not any(
            (
                differences,
                matched["unmatched_prediction"],
                matched["unmatched_target"],
                matched["ambiguous"],
                predicted["issues"],
                target["issues"],
            )
        ),
        "resources": _compare_resource_sharing(predicted, target, common),
        "dependencies": compare_precedence(predicted, target, common, details=details),
        "full_dependency_coverage": False,
        "scope": predicted["scope"],
    }


def _compare_tensor_operations(predicted: dict[str, Any], target: dict[str, Any], *, details: bool) -> dict[str, Any]:
    """Compare generated tensor work with independently observed target calls.

    Retained calls stay in the native phase comparison. CPU preparation and
    control work are not disguised as observed tensor operations here.
    """

    generated_calls = {row["call_identity"] for row in predicted["operators"]}
    expected = [
        row
        for row in predicted["operators"]
        if row["execution"] in {"async_device", "blocking_readback", "direct_blocking_device"}
    ]
    actual = [row for row in target["operators"] if row["call_identity"] in generated_calls]
    matched = _correspondence(expected, actual, "operator_identity")
    differences = [
        {
            "operator_identity": identity,
            "fields": [
                field
                for field in ("operation", "implementation", "features")
                if matched["predicted"][identity][field] != matched["actual"][identity][field]
            ],
        }
        for identity in matched["common"]
    ]
    differences = [row for row in differences if row["fields"]]

    # Count actual generated/observed device nodes, not the expected program
    # length. Equal tensor envelopes can conceal missing internal launches.
    # Readback intentionally represents synchronous transfer on the caller,
    # so its device-node count is not comparable with an async lowering.
    native_common = [
        identity
        for identity in matched["common"]
        if matched["predicted"][identity]["execution"] in {"async_device", "direct_blocking_device"}
    ]
    native_differences = [
        {
            "operator_identity": identity,
            "predicted_device_nodes": matched["predicted"][identity]["native_device_nodes"],
            "observed_device_nodes": matched["actual"][identity]["native_device_nodes"],
        }
        for identity in native_common
        if matched["predicted"][identity]["native_device_nodes"] != matched["actual"][identity]["native_device_nodes"]
    ]
    completion_common = (
        set(matched["common"]) & set(predicted["completion_identities"]) & set(target["completion_identities"])
    )
    return {
        "scope": target["scope"],
        "generated_call_count": len(generated_calls),
        "unresolved_prediction_ranks": sum(row["logical_input"] < 0 for row in predicted["operators"]),
        "predicted_tensor_count": len(expected),
        "observed_tensor_count": len(actual),
        "matched": len(matched["common"]),
        "exact_matched_work": len(matched["common"]) - len(differences),
        "differences": differences,
        "native_device_counts": {
            "compared_operators": len(native_common),
            "differences": native_differences,
            "exact_counts": bool(native_common) and not native_differences,
            "scope": "device-node multiplicity within matched async/direct calls; not native operation identity or dependency equivalence",
        },
        **{
            field: matched[field]
            for field in ("unmatched_prediction", "unmatched_target", "ambiguous", "invalid_identity")
        },
        "target_observation_coverage": {
            field: target[field]
            for field in (
                "unavailable_call_programs",
                "unmatched_program_operators",
                "overlapping_ownership",
                "source_observation_issues",
                "unassigned_call_device_nodes",
                "unassigned_device_node_kinds",
            )
        },
        "full_dependency_coverage": False,
        "resources": _compare_resource_sharing(predicted, target, completion_common),
        "dependencies": {
            **compare_precedence(predicted, target, completion_common, details=details),
            "scope": "observed DAG reachability between matched tensor completions; not internal launches or CPU/control correspondence",
            "compared_operators": len(completion_common),
            "matched_operators_without_both_completions": len(matched["common"]) - len(completion_common),
        },
    }


def _compare_allocator_work(predicted: dict[str, Any], actual: dict[str, Any]) -> dict[str, Any]:
    """Compare lengths, index regions and inner allocator queue transitions.

    Decode is paired by request and iteration. Repeated prefill chunks remain
    ambiguous. Missing observations never count as an exact empty workload.
    """

    def requests(document: dict[str, Any]) -> list[dict[str, Any]]:
        rows: list[dict[str, Any]] = []
        for call in document["calls"]:
            batch_ids = [request["request_id"] for request in call["requests"]]
            for request in call["requests"]:
                identity = (
                    f"{call['logical_input']}|{request['request_id']}|{call['phase']}|{request['decode_iteration']}"
                    if call["logical_input"] >= 0 and request["request_id"]
                    else ""
                )
                rows.append(
                    {
                        **request,
                        "request_identity": identity,
                        "batch_requests": batch_ids,
                        **{
                            field: call[field]
                            for field in (
                                "phase",
                                "page_size_tokens",
                                "batch_size",
                                "extend_tokens",
                                "new_pages",
                                "queue_state",
                            )
                        },
                    }
                )
        return rows

    matched = _correspondence(requests(predicted), requests(actual), "request_identity")
    differences = [
        {
            "request_identity": key,
            "fields": [
                field
                for field in matched["actual"][key]
                if field != "request_identity"
                and not (field == "queue_state" and matched["actual"][key][field] is None)
                and matched["actual"][key][field] != matched["predicted"][key][field]
            ],
        }
        for key in matched["common"]
    ]
    differences = [row for row in differences if row["fields"]]
    unobservable_prediction_queues = sum(call["queue_state"] is None for call in predicted["calls"])
    return {
        "matched": len(matched["common"]),
        "unmatched_prediction": matched["unmatched_prediction"],
        "unmatched_target": matched["unmatched_target"],
        "ambiguous": matched["ambiguous"],
        "invalid_identity": matched["invalid_identity"],
        "differences": differences,
        "unobservable_target_calls": actual["unobservable_calls"],
        "unobservable_target_queue_calls": actual["unobservable_queue_calls"],
        "unobservable_prediction_queue_calls": unobservable_prediction_queues,
        "work_exact_within_scope": bool(matched["common"])
        and not any(
            (
                matched["unmatched_prediction"],
                matched["unmatched_target"],
                matched["ambiguous"],
                matched["invalid_identity"],
                differences,
                actual["unobservable_calls"],
                actual["unobservable_queue_calls"],
                unobservable_prediction_queues,
            )
        ),
        "scope": "allocation lengths, batch membership, index regions and queue transitions; not emitted operators or dependencies",
    }


def _compact_comparison(report: dict[str, Any]) -> None:
    """Keep coverage and difference counts; full identity lists are opt-in diagnostics."""

    def identities(block: dict[str, Any]) -> None:
        for field in ("unmatched_prediction", "unmatched_target", "ambiguous"):
            block[field + "_count"] = len(block.pop(field))

    effects = report["effects"]
    identities(effects)
    effects["invalid_identity_count"] = len(effects.pop("invalid_identity"))
    effects["difference_count"] = len(effects["differences"])
    effects["differing_fields"] = dict(Counter(field for row in effects.pop("differences") for field in row["fields"]))
    unobservable = effects.pop("unobservable")
    effects["unobservable_count"] = len(unobservable)
    effects["unobservable_reasons"] = dict(Counter(row["reason"] for row in unobservable))

    for phase in (
        report["phases"]["prefills"],
        report["phases"]["decodes"],
        report["allocator_work"],
        report["phase_tensor_operations"],
        report["allocator_tensor_operations"],
        report["collective_operations"],
    ):
        identities(phase)
        phase["invalid_identity_count"] = len(phase.pop("invalid_identity"))
        phase["difference_count"] = len(phase["differences"])
        phase["differing_fields"] = dict(Counter(field for row in phase.pop("differences") for field in row["fields"]))

    identities(report["phase_operators"])
    for tensor in (report["phase_tensor_operations"], report["allocator_tensor_operations"]):
        comparison = tensor["native_device_counts"]
        comparison["difference_count"] = len(comparison.pop("differences"))

    dependencies = report["dependencies"]
    for field in ("missing_edge_labels", "extra_edge_labels"):
        dependencies[field + "_count"] = sum(row["count"] for row in dependencies.pop(field))
    for family in (
        "phase_operators",
        "phase_tensor_operations",
        "allocator_tensor_operations",
        "collective_operations",
    ):
        resources = report[family]["resources"]
        for field in (
            "missing_groups",
            "extra_groups",
            "prediction_operators_without_resource",
            "target_operators_without_resource",
        ):
            resources[field + "_count"] = len(resources.pop(field))
    report["diagnostic_details_retained"] = False


def _resource_partition(report: dict[str, Any], common: set[str]) -> set[tuple[str, ...]]:
    """Lane-sharing groups among matched operations, independent of lane ids."""

    return {
        tuple(sorted(common.intersection(group["operators"])))
        for group in report["resource_groups"]
        if common.intersection(group["operators"])
    }


def _compare_resource_sharing(
    predicted: dict[str, Any],
    target: dict[str, Any],
    common: set[str],
) -> dict[str, Any]:
    predicted_groups = _resource_partition(predicted, common)
    target_groups = _resource_partition(target, common)
    predicted_assigned = {identity for group in predicted_groups for identity in group}
    target_assigned = {identity for group in target_groups for identity in group}
    missing_prediction = common - predicted_assigned
    missing_target = common - target_assigned

    return {
        "scope": "lane sharing among matched operations; not physical resource identity or unowned work",
        "compared_operators": len(common),
        "compared_prediction_groups": len(predicted_groups),
        "compared_target_groups": len(target_groups),
        "sharing_exact_within_scope": bool(common)
        and not missing_prediction
        and not missing_target
        and predicted_groups == target_groups,
        "prediction_operators_without_resource": sorted(missing_prediction),
        "target_operators_without_resource": sorted(missing_target),
        "missing_groups": [list(group) for group in sorted(target_groups - predicted_groups)],
        "extra_groups": [list(group) for group in sorted(predicted_groups - target_groups)],
        "full_resource_coverage": False,
    }


def _native_phase_view(report: dict[str, Any], generated_calls: set[str], collective_nodes: set[int]) -> dict[str, Any]:
    """Separate regenerated tensor calls from retained native work for scoring.

    Reindex occurrences after partitioning on both sides. Otherwise an added
    cast inside generated metadata shifts the identity of an unrelated native
    cast. Dependencies still traverse removed calls; no model graph is changed.
    Unknown/ambiguous ownership stays in the native view, visibly unmatched.
    """

    rows = [
        row
        for row in report["operators"]
        if row.get("tensor_call_identity", "") not in generated_calls and row["graph_node"] not in collective_nodes
    ]
    counts: Counter[tuple[Any, ...]] = Counter()
    identities: dict[str, str] = {}
    operators: list[dict[str, Any]] = []
    for row in rows:
        family = tuple(
            row[key]
            for key in (
                "logical_input",
                "request_id",
                "phase",
                "iteration",
                "operation",
                "implementation",
                "resource_kind",
                "submission_implementation",
            )
        )
        occurrence = counts[family]
        counts[family] += 1
        identity = json.dumps([*family, occurrence], ensure_ascii=False, separators=(",", ":"))
        if row["implementation"] == "Node@launch" and row["submission_implementation"] is None:
            # A missing queue owner cannot safely match another missing owner.
            # Keep the row and its graph paths, but report invalid correspondence.
            identity = ""
        identities[row["operator_identity"]] = identity
        operators.append(dict(row, operator_identity=identity, occurrence=occurrence))

    dependencies = [
        dict(edge, **{"from": identities[edge["from"]], "to": identities[edge["to"]]})
        for edge in report["dependencies"]
        if edge["from"] in identities and edge["to"] in identities
    ]
    resources = [
        {"operators": [identities[identity] for identity in group["operators"] if identity in identities]}
        for group in report["resource_groups"]
    ]
    return dict(
        report,
        operators=operators,
        dependencies=dependencies,
        resource_groups=resources,
        # Removed names remain anonymous intermediates in the same dependency
        # graph. Do not enumerate pairs merely to rename the retained endpoints.
        precedence_identities=[identities.get(name) for name in report["precedence_identities"]],
        redirected_tensor_call_nodes=sum(
            row.get("tensor_call_identity", "") in generated_calls for row in report["operators"]
        ),
        redirected_collective_nodes=sum(row["graph_node"] in collective_nodes for row in report["operators"]),
        redirected_cpu_nodes=sum(
            row["resource_kind"] == "cpu_submission"
            for row in report["operators"]
            if row.get("tensor_call_identity", "") in generated_calls
        ),
    )


def _compare_phase_operators(
    predicted: dict[str, Any],
    target: dict[str, Any],
    generated_calls: set[str],
    predicted_collectives: dict[str, Any],
    target_collectives: dict[str, Any],
    *,
    details: bool,
) -> dict[str, Any]:
    """Compare owned operations and reachability through intermediate work.

    Repeated implementations use within-family occurrence, not trace node ids.
    This cannot establish correspondence for reordered indistinguishable calls
    or prove identity/resource equivalence inside an unowned worker/control path.
    """
    # Each side's local node ids only select its own native rows. Cross-trace
    # communication matching happens separately by semantic identity; unknown
    # ownership remains in the native comparison instead of disappearing.
    predicted = _native_phase_view(
        predicted, generated_calls, {row["graph_node"] for row in predicted_collectives["operators"]}
    )
    target = _native_phase_view(target, generated_calls, {row["graph_node"] for row in target_collectives["operators"]})
    matched = _correspondence(predicted["operators"], target["operators"], "operator_identity")
    common = set(matched["common"])

    def edges(report: dict[str, Any]) -> Counter[tuple[str, str, str]]:
        return Counter((edge["from"], edge["to"], edge["kind"]) for edge in report["dependencies"])

    predicted_edges, target_edges = edges(predicted), edges(target)
    comparable_prediction = Counter(
        {edge: count for edge, count in predicted_edges.items() if edge[0] in common and edge[1] in common}
    )
    comparable_target = Counter(
        {edge: count for edge, count in target_edges.items() if edge[0] in common and edge[1] in common}
    )

    def differences(values: Counter[tuple[str, str, str]]) -> list[dict[str, Any]]:
        return [
            dict(from_operator=start, to_operator=end, kind=kind, count=count)
            for (start, end, kind), count in sorted(values.items())
        ]

    # A mutation label records how the predictor created an edge, not a new
    # execution dependency. Compare operation pairs separately from raw labels.
    return {
        "scope": predicted["scope"],
        "correspondence": predicted["correspondence"],
        "partition": "Generated tensor calls and attributed collectives use their semantic comparisons; CPU/control and communication protocol internals remain unproven. Unknown ownership remains native.",
        "redirected_prediction_collective_nodes": predicted["redirected_collective_nodes"],
        "redirected_target_collective_nodes": target["redirected_collective_nodes"],
        "redirected_target_tensor_call_nodes": target["redirected_tensor_call_nodes"],
        "redirected_target_cpu_nodes_without_internal_comparison": target["redirected_cpu_nodes"],
        "predicted_count": len(predicted["operators"]),
        "target_count": len(target["operators"]),
        "matched_count": len(common),
        "prediction_worker_submission_unresolved": sum(
            row["implementation"] == "Node@launch" and row["submission_implementation"] is None
            for row in predicted["operators"]
        ),
        "target_worker_submission_unresolved": sum(
            row["implementation"] == "Node@launch" and row["submission_implementation"] is None
            for row in target["operators"]
        ),
        "unmatched_prediction": matched["unmatched_prediction"],
        "unmatched_target": matched["unmatched_target"],
        "ambiguous": matched["ambiguous"],
        "predicted_repeated_ownership": predicted["repeated_ownership"],
        "target_repeated_ownership": target["repeated_ownership"],
        "predicted_inactive_work": predicted["inactive_work"],
        "target_inactive_work": target["inactive_work"],
        "predicted_unassigned_active_nodes": predicted["unassigned_active_nodes"],
        "target_unassigned_active_nodes": target["unassigned_active_nodes"],
        "resources": _compare_resource_sharing(predicted, target, common),
        "dependencies": {
            **compare_precedence(predicted, target, common, details=details),
            "status": "PARTIAL_COVERAGE",
            "full_dependency_coverage": False,
            "compared_prediction_edges": sum(comparable_prediction.values()),
            "compared_target_edges": sum(comparable_target.values()),
            "missing_edge_labels": differences(comparable_target - comparable_prediction),
            "extra_edge_labels": differences(comparable_prediction - comparable_target),
            "prediction_edges_with_unmatched_endpoint": sum(predicted_edges.values())
            - sum(comparable_prediction.values()),
            "target_edges_with_unmatched_endpoint": sum(target_edges.values()) - sum(comparable_target.values()),
            "uncompared_prediction_incident_edges": predicted["uncompared_incident_edges"],
            "uncompared_target_incident_edges": target["uncompared_incident_edges"],
            "uncompared_prediction_other_edges": predicted["uncompared_other_edges"],
            "uncompared_target_other_edges": target["uncompared_other_edges"],
            "reason": "Phase precedence is compared; intermediate operation identities, resource equivalence and the full HiCache graph remain unproven.",
        },
    }
