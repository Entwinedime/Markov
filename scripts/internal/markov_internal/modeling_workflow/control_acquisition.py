"""Shared control acquisition through the existing capture, CPU and export tools."""

from __future__ import annotations

from collections.abc import Sequence
import subprocess
from pathlib import Path
from typing import TYPE_CHECKING, Any

from ..common.io import load_json, write_json
from ..common.paths import ROOT_DIR, require_repo_path, repo_relative_path
from ..modeling.cost.models import ModelSpec
from .capture import CONTROL_CAPTURE_STAGES, capture_calibration
from ..modeling.cost.control import control_calibrations
from .group_cpu_service import prepare_group_cpu_service
from ..modeling.source_dag.profiles import ProfileRunRef, discover_profile_runs, parse_server_command_flags

if TYPE_CHECKING:
    from .group import GroupRequest


PREFETCH_CAPTURE_POLICIES = {
    "local_return": "best_effort",
    "best_effort": "best_effort",
    "wait_complete": "wait_complete",
}


def operator_requirements(needs: Sequence[dict], operation: str) -> list[dict]:
    """Select operation descriptors inside grouped native cost gaps."""
    return [
        need for need in needs if any(coordinate.get("operation") == operation for coordinate in need["coordinates"])
    ]


def declared_cost_demands(needs: Sequence[dict], models: Sequence[dict]) -> tuple[list[dict], list[dict], list[dict]]:
    """Separate missing measurements from operations with no training definition.

    Return collectable demands, undeclared models and demands requiring a
    descriptor/model correction. Acquisition cannot invent missing target
    variables or repair an invalid numerical response by repeating a workload.
    Non-operator demands keep their existing collection path.
    """

    # Training declarations are external input. Reject an unsupported form
    # before starting a capture, using the offline trainer's same contract.
    specifications = [ModelSpec.from_raw(model) for model in models]
    collectable, undeclared, corrections = [], [], []
    for need in needs:
        if need["component"].rsplit("/", 1)[-1] in {"feature", "model"}:
            corrections.append(need)
            continue

        coordinates = []
        incomplete_work = []
        ambiguous_work = []
        for work in need["coordinates"]:
            if "operation" not in work:
                coordinates.append(work)
                continue

            definitions = [
                model
                for model in specifications
                if (
                    model.operation == work["operation"]
                    and model.implementation in {"*", work["implementation"]}
                    and model.timing_scope == work["timing_scope"]
                )
            ]
            if not definitions:
                if work not in undeclared:
                    undeclared.append(work)
                continue

            # The trainer rejects overlapping wildcard/explicit definitions;
            # acquisition cannot resolve which model the user intended.
            if len(definitions) != 1:
                ambiguous_work.append(work)
                continue

            # A model may not exist yet, so native inference cannot report
            # its missing features. The explicit training definition already
            # tells us whether this target descriptor could ever query it.
            model = definitions[0]
            missing = sorted(
                {feature for feature in (*model.features, *model.coverage_features) if feature not in work["features"]}
            )
            if missing:
                incomplete_work.append(dict(work, missing_features=missing))
            else:
                coordinates.append(work)

        if coordinates:
            collectable.append(dict(need, coordinates=coordinates))
        if incomplete_work:
            corrections.append(
                dict(
                    need,
                    coordinates=incomplete_work,
                    reason="Declared model variables are missing from target work; acquisition cannot supply target descriptors.",
                )
            )
        if ambiguous_work:
            corrections.append(
                dict(
                    need,
                    coordinates=ambiguous_work,
                    reason="Overlapping training declarations match this work; select one model per operation, implementation and timing scope before acquisition.",
                )
            )
    return collectable, undeclared, corrections


def pending_control_experiments(needs: Sequence[dict], attempted: set[str]) -> list[str]:
    """Map execution demands to shared experiments, with queries exported last.

    The caller owns the attempt lifecycle. Unknown programs stay unresolved;
    they are not redirected to a different measurement.
    """
    components = {need["component"] for need in needs}
    operations = [
        name
        for name in ("eviction_locked_candidate", "release_regular")
        if "execution_control/" + name in components and name not in attempted
    ]
    if operator_requirements(needs, "eviction.locked_candidate") and "eviction_locked_candidate" not in attempted:
        if "eviction_locked_candidate" not in operations:
            operations.append("eviction_locked_candidate")
    # Describe experiments by execution policy, not by the target cell. One
    # write-back pressure capture measures both submission and backup release.
    for need in needs:
        for coordinate in need["coordinates"]:
            operation = coordinate.get("operation", "")
            implementation = coordinate.get("implementation", "")
            experiment = None
            if operation.startswith("write.submission."):
                if implementation == "kernel_ascend.page_first_direct.write_back":
                    experiment = "write_back"
                elif implementation == "kernel_ascend.page_first_direct.write_through":
                    experiment = "write_through"
            elif operation.startswith("load.submission."):
                if implementation in {"kernel_ascend.page_first_direct.load", "empty_queue"}:
                    experiment = "wait_complete"
            elif operation.startswith("write.confirmation.") or operation == "runtime.event_destroy":
                experiment = "write_through"
            elif (
                (operation == "write.completion_entry" and implementation in {"blocking", "blocking_empty"})
                or (
                    (operation == "write.completion_advance" or operation.startswith("write.completion."))
                    and implementation == "blocking"
                )
                or (operation == "write.completion_poll" and implementation == "blocking_storage")
            ):
                experiment = "write_back"
            elif operation.startswith("load.index.") and implementation == "ascend.paged_unsorted":
                experiment = "wait_complete"
            elif operation == "load.admission.prepare" and implementation == "host_indices_cpu":
                experiment = "wait_complete"
            elif (
                operation in {"load.admission.prefix_concat", "load.admission.prefix_tail"}
                and implementation == "ascend.aclnn_cat"
            ):
                experiment = "wait_complete"
            elif operation.startswith("release."):
                if implementation in {"ascend.paged_immediate.backup", "ascend.token_immediate.backup"}:
                    experiment = "write_back"
                elif implementation in {"ascend.paged_immediate.regular", "ascend.token_immediate.regular"}:
                    experiment = "release_regular"
            elif implementation == "nonblocking":
                if operation == "write.confirmation_tail":
                    experiment = "write_through"
                elif operation == "load.confirmation_tail":
                    experiment = "wait_complete"
            elif operation == "layer_wait.inactive_return" and implementation == "inactive":
                experiment = "wait_complete"
            elif operation in {"layer_wait.submit", "layer_wait.tail"} and implementation == "ascend.stream_wait_event":
                experiment = "wait_complete"
            elif (operation, implementation) in {
                ("device.event.wait", "ascend.EVENT_WAIT"),
                ("device.event.record", "ascend.EVENT_RECORD"),
            }:
                # Runtime extraction shares raw device events across callers;
                # this experiment still measures each caller's CPU separately.
                experiment = "wait_complete"
            elif operation.startswith("prefetch.check.") and implementation == "sglang.prefetch_check":
                role = operation.removeprefix("prefetch.check.")
                experiment = (
                    "best_effort" if role in {"no_operation_return", "best_effort_to_stop"} else "wait_complete"
                )
            elif operation.startswith("prefetch.collective.") and implementation == "gloo.all_reduce.int32":
                experiment = "wait_complete"
            elif operation.startswith("prefetch.scheduler.") and implementation in {
                "sglang.serial_scheduler",
                "gloo.broadcast.int64",
                "gloo.all_reduce.int32",
            }:
                experiment = "wait_complete"
            if experiment is not None and experiment not in attempted and experiment not in operations:
                operations.append(experiment)
    waits = {
        PREFETCH_CAPTURE_POLICIES[coordinate["program"]]
        for need in needs
        if need["component"] == "execution_control/prefetch"
        for coordinate in need["coordinates"]
        if coordinate["program"] in PREFETCH_CAPTURE_POLICIES
    }
    operations.extend(sorted(waits - attempted - set(operations)))

    # The wait experiment may publish a paired trace that also supplies queries.
    query_needed = "execution_control/prefetch_query" in components or bool(
        operator_requirements(needs, "prefetch.query")
    )
    if query_needed and "prefetch_query" not in attempted:
        operations.append("prefetch_query")
    return operations


def prepare_control_calibration(group: GroupRequest, demand: str, needs: Sequence[dict]) -> dict:
    """Resolve a shared control demand, then capture and export its normal CPU costs."""
    # Materialization imports group definitions; keep that dependency local.
    from .fixed_calibration import materialize_fixed_calibration

    if demand == "prefetch_query":
        operation = "prefetch_query"
    elif demand == "release_regular":
        operation = "release_host"
    elif demand in {"write_back", "write_through"}:
        operation = "write_host"
    else:
        operation = "prefetch_wait"
    if operation == "prefetch_query":
        # Group admission already checks these independent manifests and their
        # environment. Re-export with current code; do not reuse old parameters.
        for path in sorted(set(group.controls["prefetch_wait"].values())):
            existing = load_json(require_repo_path(path))
            if existing.get("cost_basis") != "paired_cpu_service" or not existing.get("cpu_service_file"):
                continue
            (profile,) = discover_profile_runs((require_repo_path(existing["source_manifest"]),))
            return _export_control(
                group,
                profile,
                require_repo_path(existing["cpu_service_file"]),
                group.output_dir / "control_calibration" / "shared_query",
                operation,
            )

    if demand == "release_regular":
        definition = materialize_fixed_calibration(group, release_only=True)
    elif demand in {"write_back", "write_through"}:
        definition = materialize_fixed_calibration(group, write_policy=demand)
    else:
        policy = "wait_complete" if demand == "prefetch_query" else PREFETCH_CAPTURE_POLICIES[demand]
        definition = materialize_fixed_calibration(group, prefetch_policy=policy)

    paired = capture_paired_calibration(group, definition)
    if paired["status"] != "paired_calibrated":
        return paired

    (profile,) = discover_profile_runs((require_repo_path(paired["profile_manifest"]),))
    service = require_repo_path(paired["cpu_service_cost"])
    output = require_repo_path(paired["output_dir"])
    exports = [operation]
    if demand == "wait_complete":
        # Several cost families use the same paired waiting experiment. Export
        # the needed observations once; do not schedule another capture for
        # each primitive or assume raw-profile import extracts these audits.
        requested = {coordinate.get("operation", "") for need in needs for coordinate in need["coordinates"]}
        if any(name.startswith(("load.index.", "load.admission.")) for name in requested):
            exports.append("load_index")
        if any(name.startswith("load.submission.") for name in requested):
            exports.append("load_submission")
        if any(name.startswith("layer_wait.") for name in requested):
            exports.append("layer_wait")
    return _export_control(group, profile, service, output, operation, extra_operations=exports[1:])


def capture_paired_calibration(group: GroupRequest, definition: dict[str, Any]) -> dict[str, Any]:
    """Capture fixed outputs and full/light replays, then bind same-capture CPU costs."""

    manifests = {}
    for stage in CONTROL_CAPTURE_STAGES:
        result = capture_calibration(group, definition, stage=stage)
        if result["status"] not in {"captured", "already_captured"}:
            return dict(result, stage=stage)
        manifests[stage] = result["profile_manifest"]

    source = require_repo_path(manifests["profiled_replay"])
    (profile,) = discover_profile_runs((source,))
    flags = parse_server_command_flags(profile.run_dir / "server_cmd.txt")
    tp = int(flags.get("tp_size", "1"))
    ledger_path = group.output_dir / "capture_ledger.json"
    ledger = load_json(ledger_path)
    captures = {row["profile_manifest"]: row for row in ledger["attempts"] if "profile_manifest" in row}
    full, light = (captures[manifests[stage]] for stage in ("profiled_replay", "light_replay"))
    # Attempt directories already distinguish actual measurements. Bind both
    # CPU correction and exported costs to the pair, not the reusable point name.
    output = group.output_dir / "control_calibration"
    for row in (full, light):
        output /= require_repo_path(row["suite_config"]).parent.name
    pair = dict(
        profile_manifest=manifests["profiled_replay"],
        light_manifest=manifests["light_replay"],
        tp_size=tp,
        output_dir=str(repo_relative_path(output / "cpu")),
        include_setup_requests=True,
    )
    services = prepare_group_cpu_service(dict(pairs=[pair], existing_services=[]))
    full["cpu_service_cost"] = str(repo_relative_path(services[0]))
    write_json(ledger_path, ledger)

    return dict(
        status="paired_calibrated",
        profile_manifest=manifests["profiled_replay"],
        cpu_service_cost=str(repo_relative_path(services[0])),
        output_dir=str(repo_relative_path(output)),
    )


def _export_control(
    group: GroupRequest,
    profile: ProfileRunRef,
    service: Path,
    output: Path,
    operation: str,
    *,
    extra_operations: Sequence[str] = (),
) -> dict:
    # Export only the required operation, not every operation this workload
    # happens to contain. The paired capture is shared by the whole group.
    operations = [operation, *extra_operations]
    command = [
        str(ROOT_DIR / "scripts/model.sh"),
        "export-hicache-operation-costs",
        "--profile-manifest",
        str(profile.manifest_path),
        "--cpu-service-cost",
        str(service),
        "--page-size",
        str(profile.hicache_config["page_size"]),
        "--output-dir",
        str(output),
    ]
    for name in operations:
        command.extend(("--operation", name))
    subprocess.run(command, cwd=ROOT_DIR, check=True)

    value = str(repo_relative_path(output / (operation + ".json")))
    if operation == "prefetch_wait":
        collection = {"prefetch_wait": {profile.hicache_config["prefetch_policy"]: value}}
    elif operation == "write_host":
        collection = {
            "write_host": {str(profile.hicache_config["page_size"]): {profile.hicache_config["write_policy"]: value}}
        }
    else:
        collection = {operation: value}
    for name in extra_operations:
        collection[name] = str(repo_relative_path(output / (name + ".json")))
    index = group.output_dir / "control_calibrations.json"
    write_json(index, control_calibrations(load_json(index) if index.exists() else {}, collection))
    return dict(
        status="control_calibrated",
        profile_manifest=str(repo_relative_path(profile.manifest_path)),
        control_calibrations=collection,
    )
