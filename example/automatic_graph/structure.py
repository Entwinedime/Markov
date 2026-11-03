#!/usr/bin/env python3
"""Compare a prediction with native batch, state and device evidence without changing either graph."""

from __future__ import annotations

import argparse
import json
import traceback
from collections import Counter, defaultdict, deque
from itertools import chain
from pathlib import Path
from typing import Any

from communication import CommunicationComparison
from execution import ExecutionComparison
from markov_collection.artifacts import write_json
from operations import OperationComparison, RowAccess, TargetOperations, WorkOrigins


class StructureComparison:
    def __init__(
        self, source: Path, physical: Path, observed: Path,
        prediction: dict[str, Any], specification: dict[str, Any], target: TargetOperations,
        communication: CommunicationComparison,
    ) -> None:
        self.source = json.loads(source.read_text())
        self.physical = json.loads(physical.read_text())
        self.observed = json.loads(observed.read_text())
        self.prediction = prediction
        self.target = target
        self.communication = communication
        session = json.loads((source.parent / "session.json").read_text())
        self.request_ids = dict(
            zip(session["source_requests"], (item["id"] for item in specification["requests"]), strict=True)
        )
        self.rank = self.source["environment"]["rank"]
        self.stage = self.source["environment"]["pp_rank"]
        self.native_batches = self.region_labels(self.source["regions"], True, "batch")
        self.target_batches = self.region_labels(self.prediction["graph"]["regions"], False, "batch")
        self.target_actions = [
            None if task is None else (task["group"], task["sequence"])
            for task in self.region_labels(self.prediction["graph"]["regions"], False, "task")
        ]
        self.native_requests = self.region_labels(self.source["regions"], True, "request")
        self.serving_batches = {
            batch["id"] for batch in self.source["batches"]
            if batch["observation"] == "source" and batch.get("kind") != "primitive"
        }
        self.serving_regions = self.observation_regions()
        self.native_model = self.model_regions(self.source["regions"])
        self.target_model = self.model_regions(self.prediction["graph"]["regions"])
        self.work_origins = WorkOrigins(
            prediction.get("observed_work", []), self.rank, self.source["regions"], self.target_actions,
        )
        self.native_controls = self.region_labels(self.source["regions"], True, "control_only")
        self.excluded_control, self.control_scope = self.controller_scope()

        self.decisions: dict[int, dict[str, Any]] = {}
        self.batches: dict[int, dict[str, Any]] = {}
        self.completions = {}

        # A batch can span pipeline actions; P and D select their own member groups.
        groups = {
            group["id"] for group in self.prediction["structure"]["topology"]["groups"]
            if self.rank in group["members"]
        }
        for decision in self.prediction["decisions"]:
            if decision["task"]["group"] not in groups:
                continue
            flow = decision["flow"]
            if "phase" in decision:
                self.batches[flow] = decision
            if decision["commits_requests"] or decision["step"] == "model":
                self.completions[flow] = decision
            if decision["step"] in {"batch", "model", "compute", "accept"} or (
                decision["step"] == "stage" and decision["stage"] == self.stage
            ):
                self.decisions[flow] = decision

        self.operations = OperationComparison(
            self.source, self.observed, target, self.work_origins,
            self.row_accesses(self.observed, True), self.row_accesses(self.prediction["graph"], False),
        )
        self.native_models = self.model_operations(self.observed, self.native_batches, True)
        self.target_models = self.model_operations(self.prediction["graph"], self.target_batches, False)

    def request_rows(self, region: dict[str, Any], metadata: dict[str, Any], observed: bool) -> dict[str, int] | None:
        flow = (self.native_batches if observed else self.target_batches)[region["id"]]
        if observed:
            if flow is None:
                return None
            batch = self.source["batches"][flow]
            if batch["observation"] != "source" or batch.get("kind") == "primitive":
                return None
        else:
            # Reservation and preparation carry real batch operands even when this flow has no model call.
            batch = self.batches.get(flow)
            if batch is None:
                return None

        phases = {"prefill", "decode"}
        if metadata.get("entrypoint") == "batch_preparation":
            phases = (
                {metadata["entrypoint_phase"]} if observed else
                {phase for phase in phases if metadata["parameters"][phase + "_members"]}
            )
        return {
            self.request_ids[m["id"]] if observed else m["id"]: m["slot"] if observed else m["slot"] + 1
            for m in batch["members"] if m["phase"] in phases
        }

    def row_accesses(self, graph: dict[str, Any], observed: bool) -> dict[tuple[int, int], RowAccess]:
        regions = self.source["regions"] if observed else graph["regions"]
        values = self.source["values"] if observed else graph["values"]
        metadata = [region["metadata"] if observed else region for region in regions]
        maps = {
            values[ports["request_map"]]["storage"]
            for item in metadata for ports in item.get("ports", {}).values() if "request_map" in ports
        }
        tables = maps | {
            values[ports["token_relay"]]["storage"]
            for item in metadata for ports in item.get("ports", {}).values() if "token_relay" in ports
        }
        batches = self.native_batches if observed else self.target_batches
        indices = {}
        scopes = []
        for region, item in zip(regions, metadata, strict=True):
            if not observed and region.get("rank") != self.rank:
                scopes.append(None)
                continue
            scope = None if region["parent"] is None else scopes[region["parent"]]
            slot_values = [
                ports["request_slots"] for ports in item.get("ports", {}).values() if "request_slots" in ports
            ]
            request_slot = item.get("parameters", {}).get("request_slot")
            needs_rows = slot_values or request_slot is not None or item.get("entrypoint") == "batch_preparation"
            selected = self.request_rows(region, item, observed) if needs_rows else None
            if selected is not None:
                for value in slot_values:
                    if values[value].get("shape") == [len(selected)]:
                        indices[batches[region["id"]], value] = selected

            # A partial Mixed preparation cannot claim accesses for the other part's requests.
            if item.get("entrypoint") == "batch_preparation":
                complete = selected is not None and len(selected) == item["parameters"]["B"]
                scope = (selected, set(selected.values())) if complete else None
            if request_slot is not None:

                # A request-scoped callback can run under a later batch's scheduling frame.
                request = self.native_requests[region["id"]] if observed else None
                if request is not None:
                    selected = {self.request_ids[request]: request_slot} if request in self.request_ids else None
                else:
                    selected = {name: row for name, row in (selected or {}).items() if row == request_slot} or None
                scope = selected, {request_slot}
            scopes.append(scope)

        accesses = {}
        for operation in graph["operations"]:
            scope = scopes[operation["region"]]
            indexed = None

            # These native APIs index the first table axis; identity ties the selector to an actual batch column.
            if operation["name"] in {"tensor_assignment", "aten.index.Tensor"}:
                table, index = operation["arguments"][:2]
                if isinstance(index, list):
                    index = index[0] if index else None
                if isinstance(table, dict) and "value" in table and isinstance(index, dict) and "value" in index:
                    rows = indices.get((batches[operation["region"]], index["value"]))
                    if rows is not None:
                        indexed = values[table["value"]]["storage"], rows

            assignment = operation["kind"] == "interface" and operation["name"] == "tensor_assignment"
            read_storage = set()
            if assignment:

                # A destination reused as payload or selector is still read, even when inputs deduplicate its ID.
                operands = OperationComparison.argument_values([operation["arguments"][1:], operation["keywords"]])
                read_storage = {values[value]["storage"] for value in operands.values()}

            aliases = {values[value]["storage"] for value in operation["inputs"]} & tables
            for storage in aliases:
                if indexed is not None and storage == indexed[0]:
                    selected, rows = indexed[1], set(indexed[1].values())
                elif storage in maps and scope is not None:
                    selected, rows = scope
                else:
                    continue
                writes = storage in operation["writes"]
                reads = selected
                if writes and assignment:
                    reads = None if storage in read_storage else {}
                accesses[operation["id"], storage] = RowAccess(reads, rows if writes else set())
        return accesses

    def observation_regions(self) -> set[int]:
        request_lists = self.region_labels(self.source["regions"], True, "requests")
        result = set()
        for region in self.source["regions"]:
            identity = region["id"]
            batch = self.native_batches[identity]
            if batch is not None:

                # Original batch labels decide qualification, including prototypes that reuse serving request names.
                if batch in self.serving_batches:
                    result.add(identity)
                continue
            requests = request_lists[identity] or [self.native_requests[identity]]
            if requests and set(requests) <= self.request_ids.keys():
                result.add(identity)
        return result

    @staticmethod
    def region_labels(regions: list[dict[str, Any]], observed: bool, field: str) -> list[Any]:
        result = []
        for region in regions:
            batch = (region["metadata"] if observed else region).get(field)
            if batch is None and region["parent"] is not None:
                batch = result[region["parent"]]
            result.append(batch)
        return result

    @staticmethod
    def model_regions(regions: list[dict[str, Any]]) -> list[bool]:
        result = []
        for region in regions:
            result.append(
                region["role"] in {"Prefill", "Decode", "Mixed"}
                or (region["parent"] is not None and result[region["parent"]])
            )
        return result

    def signature(self, batch: dict[str, Any], observed: bool, prebuilt: bool = False) -> tuple[Any, ...]:
        members = []
        for member in batch["members"]:
            identity = self.request_ids[member["id"]] if observed else member["id"]
            members.append(identity if prebuilt else (identity, member["new_tokens"], member["context_tokens"]))
        return "prebuilt" if prebuilt else batch["phase"], tuple(members)

    def model_operations(
        self, graph: dict[str, Any], batches: list[int | None], observed: bool
    ) -> dict[int | None, list[dict[str, Any]]]:
        operations = defaultdict(list)
        modeled = self.native_model if observed else self.target_model
        for operation in graph["operations"]:
            region = operation["region"]
            if not modeled[region]:
                continue
            if observed:
                if region not in self.serving_regions or operation["id"] in self.excluded_control:
                    continue
            elif graph["regions"][region].get("rank") != self.rank:
                continue
            operations[batches[region]].append(operation)
        return operations

    def native_region(self, record: dict[str, Any]) -> int | None:
        region = record["region"]
        if region is None and record["operation"] is not None:
            region = self.source["operations"][record["operation"]]["region"]
        return region

    def controller_scope(self) -> tuple[set[int], dict[str, Any]]:
        operations = [
            operation for operation in self.observed["operations"] if self.native_controls[operation["region"]]
        ]
        identities = {operation["id"] for operation in operations}
        crossing = sum(
            (edge["source"] in identities) != (edge["target"] in identities)
            for edge in self.observed["dependencies"]
        )
        actions = chain(
            (work for work in self.physical["work"] if work["kind"] == "device"),
            self.physical["runtime_actions"],
        )

        # Some runtime actions belong directly to a region, without an attached logical operation.
        submitted_work = sum(
            1
            for action in actions
            if not action["observer"]
            and not action["auxiliary"]
            and action["baseline"] is None
            and (region := self.native_region(action)) is not None
            and self.native_controls[region]
        )
        communications = sum(bool(operation.get("communication")) for operation in operations)
        closed = not crossing and not submitted_work and not communications

        # A declaration cannot hide data consumed by shared work or an unrepresented device/API dependency.
        return (identities if closed else set()), {
            "closed": closed,
            "declared_operations": len(operations),
            "crossing_dependencies": crossing,
            "device_and_runtime_actions": submitted_work,
            "communication_contracts": communications,
            "native_calls": sorted(
                {
                    region["metadata"]["path"]
                    for region in self.source["regions"]
                    if region["metadata"].get("control_only")
                }
            ),
            "scope": "declared native controller decisions; retained in observations, separate from modeled work",
        }

    def native_work(self, model_only: bool = False) -> dict[int | None, Counter[str]]:
        result: dict[int | None, Counter[str]] = defaultdict(Counter)
        for records, device in ((self.physical["work"], True), (self.physical["runtime_actions"], False)):
            for record in records:
                if record["observer"] or record["auxiliary"] or record["baseline"] is not None:
                    continue
                if device:
                    if record["kind"] != "device" or record["task_type"] in ("EVENT_RECORD", "EVENT_WAIT"):
                        continue
                    operation = record["operation"]
                    if operation is not None and self.source["operations"][operation].get("communication", False):
                        continue
                    name = record["name"]
                else:

                    # An async copy can lack a profiler task; its native API is still an observable boundary.
                    if record["kind"] != "copy_async":
                        continue
                    name = "copy_async:" + record["native_api"]

                region = self.native_region(record)
                if (
                    region is None or region not in self.serving_regions
                    or (model_only and not self.native_model[region])
                ):
                    continue
                if self.control_scope["closed"] and self.native_controls[region]:
                    continue

                # Request-owned transport remains in the window even without a model-batch label.
                result[self.native_batches[region]][name] += 1
        return result

    def target_work(self, model_only: bool = False) -> dict[int | None, Counter[str]]:
        graph = self.prediction["graph"]
        result: dict[int | None, Counter[str]] = defaultdict(Counter)
        for node in graph["execution"]["nodes"]:
            if node["kind"] not in ("device", "copy_async"):
                continue
            region = node.get("region")
            if region is None:
                region = graph["operations"][node["operation"]]["region"]
            batch = self.target_batches[region]
            if (
                graph["regions"][region].get("rank") == self.rank
                and (not model_only or self.target_model[region])
            ):
                name = node["name"] if node["kind"] == "device" else "copy_async:" + node["name"]
                result[batch][name] += 1
        return result

    @staticmethod
    def compare_work(report: dict[str, Any], native: Counter[str], target: Counter[str]) -> None:
        report["checks"]["compute_and_async_copies"] = native == target
        if native != target:
            report["missing_work"] = dict(native - target)
            report["extra_work"] = dict(target - native)

    def state(self, batch: dict[str, Any], flow: int) -> dict[str, bool | None]:
        regions = [region for region in self.source["regions"] if self.native_batches[region["id"]] == batch["id"]]
        (result,) = [region for region in regions if region["role"] in {"ResultProcessed", "PrebuiltResult"}]
        graph = self.prediction["graph"]
        (target,) = [
            region
            for region in graph["regions"]
            if region["role"] == result["role"] and region.get("rank") == self.rank and region.get("batch") == flow
        ]
        decision = self.decisions[flow]
        completion = self.completions.get(flow)
        values = self.source["values"]
        ports = result["metadata"].get("ports", {})
        native_effects = [
            (self.request_ids[item["id"]], item["generated_tokens"], item["finished"])
            for item in result["metadata"]["effects"]
        ]
        target_effects = (
            [(item["id"], item["generated_tokens"], item["finished"]) for item in completion["members"]]
            if completion is not None
            else None
        )

        # A later KV release belongs to the flow, but not to this model-result callback's effects.
        result_regions = {result["id"]}
        for region in regions:
            if region["parent"] in result_regions:
                result_regions.add(region["id"])
        released = [
            region["metadata"].get("observed_result")
            for region in regions
            if region["id"] in result_regions and region["metadata"].get("interface") == "tensor_unique"
        ]

        # A result can return pages through nested cache actions before its request-completion action.
        target_regions = {target["id"]}
        for region in graph["regions"]:
            if region["parent"] in target_regions:
                target_regions.add(region["id"])
        result_actions = {self.target_actions[region] for region in target_regions}
        if completion is not None:
            result_actions.add((completion["task"]["group"], completion["task"]["sequence"]))
        returned = Counter(
            page + 1
            for event in self.prediction["resource_history"]
            if event["kind"] == "return_pages"
            and (event["task"]["group"], event["task"]["sequence"]) in result_actions
            for page in event["pages"]
        )

        checks = {
            "slots": [item["slot"] for item in batch["members"]] == [item["slot"] + 1 for item in decision["members"]],
            "effects": native_effects == target_effects if target_effects is not None else None,
            "released_page_ids": Counter(chain.from_iterable(released)) == returned,
        }

        # Pool ports describe this rank's state; a PD decision's global free-page count belongs to Decode.
        for direction, boundary in (("inputs", "before"), ("outputs", "after")):
            left = ports.get(direction, {}).get("free_pages")
            right = target.get("ports", {}).get(direction, {}).get("free_pages")
            checks[f"free_pages_{boundary}_result"] = (
                values[left]["shape"] == graph["values"][right]["shape"]
                if left is not None and right is not None
                else None
            )
        return checks

    def window(self, operation_pairs: dict[int, dict[str, Any]]) -> dict[str, Any]:
        # Source membership includes empty decisions and final cleanup; auxiliary prototypes are separate work.
        native = [
            operation
            for operation in self.observed["operations"]
            if operation["id"] not in self.excluded_control
            and operation["region"] in self.serving_regions
        ]
        graph = self.prediction["graph"]
        target = [
            operation
            for operation in graph["operations"]
            if graph["regions"][operation["region"]].get("rank") == self.rank
        ]
        known = {
            operation["id"]: operation_pairs[operation["id"]]
            for operation in native
            if operation["id"] in operation_pairs
        }
        work = self.work_origins.bind(native, target, self.operations, known)
        pairs, correspondence = self.operations.correspondence(native, target, known)
        correspondence.update(self.operations.caller_order(native, pairs))
        paired_native = [operation for operation in native if operation["id"] in pairs]
        paired = [pairs[operation["id"]] for operation in paired_native]
        window = {
            "scope": (
                "rank-local source serving observations and request-owned transport, including empty decisions "
                "and terminal cleanup; baselines, auxiliary prototypes and other ranks excluded"
            ),
            "native_operations": len(native),
            "target_operations": len(target),
            "correspondence": correspondence,
            "observed_work": work,
            "source_boundary": self.source["environment"].get("observation_window"),
            "initial_source_lane": next(
                (batch["lane"] for batch in self.source["batches"] if batch["id"] in self.serving_batches), None
            ),
            "initial_target_lane": next(
                (region["lane"] for region in graph["regions"] if region.get("rank") == self.rank), None
            ),
            "communication": self.communication.observe(self.source, self.physical, native, pairs),
        }
        if not paired:
            return {**window, "status": "partial", "reason": "no paired operations in the window"}

        # Unpaired work limits coverage, but cannot hide dependencies between paired owners.
        report = self.operations.compare(paired_native, paired)
        report["execution"] = ExecutionComparison(
            paired_native, paired, graph["execution"], self.physical, self.target.execution_paths
        ).compare()
        report["checks"]["execution_relations"] = report["execution"]["same"]
        report["checks"]["consistent_bindings"] = correspondence["consistent_bindings"]
        report["checks"]["observed_work_ownership"] = (
            False if work["conflicting_bindings"] else self.work_origins.consistent(pairs, native)
        )

        # Raw window totals also cover work omitted from a projection; scheduling scopes need not coincide.
        self.compare_work(
            report,
            sum(self.native_work().values(), Counter()),
            sum(self.target_work().values(), Counter()),
        )
        checks = report["checks"]
        status = "matched" if correspondence["complete"] and None not in checks.values() else "partial"
        if False in checks.values():
            status = "differences"
        return {
            **window,
            **report,
            "same_operation_order": correspondence["same_serialized_order"],
            "status": status,
        }

    def model_body(
        self, batch: int, flow: int, native_work: Counter[str], target_work: Counter[str],
    ) -> tuple[dict[str, Any], dict[int, dict[str, Any]]]:
        native = self.native_models.get(batch, [])
        target = self.target_models.get(flow, [])
        pairs, correspondence = self.operations.correspondence(native, target)
        correspondence.update(self.operations.caller_order(native, pairs))
        matched = [operation for operation in native if operation["id"] in pairs]
        predicted = [pairs[operation["id"]] for operation in matched]
        report = {
            **self.operations.compare(matched, predicted),
            "correspondence": correspondence,
            "native_operations": len(native),
            "target_operations": len(target),
        }
        report["checks"].update(
            call_correspondence=True if correspondence["complete"] else None,
            consistent_bindings=correspondence["consistent_bindings"],
        )
        if pairs:
            report["execution"] = ExecutionComparison(
                matched, predicted, self.prediction["graph"]["execution"], self.physical, self.target.execution_paths,
            ).compare()
            report["checks"]["execution_relations"] = report["execution"]["same"]
        self.compare_work(report, native_work, target_work)

        # Only a complete model correspondence anchors the whole-window comparison.
        return report, pairs if correspondence["complete"] else {}

    def compare(self) -> dict[str, Any]:
        target: dict[tuple[Any, ...], deque[int]] = defaultdict(deque)
        for flow, decision in self.decisions.items():
            target[self.signature(decision, False, decision["step"] == "accept")].append(flow)
        native_model_work = self.native_work(model_only=True)
        target_model_work = self.target_work(model_only=True)
        comparisons = []
        operation_pairs = {}
        unmatched = []
        serving_batches = {
            self.native_batches[region["id"]]: region["role"]
            for region in self.source["regions"]
            if region["role"] in {"ResultProcessed", "PrebuiltResult"}
        }

        for batch in self.source["batches"]:
            if batch["id"] not in self.serving_batches or batch["id"] not in serving_batches:
                continue
            key = self.signature(batch, True, serving_batches[batch["id"]] == "PrebuiltResult")
            if not target[key]:
                unmatched.append(batch["id"])
                continue
            index = target[key].popleft()
            comparison = {"native_batch": batch["id"], "target_flow": index, "checks": self.state(batch, index)}

            # Model reuse is checked separately; different control paths do not certify or refute its body.
            if self.decisions[index]["step"] != "accept":
                comparison["model_body"], model_pairs = self.model_body(
                    batch["id"], index, native_model_work[batch["id"]], target_model_work[index],
                )
                operation_pairs.update(model_pairs)
            comparisons.append(comparison)

        unmatched_target = sorted(index for indices in target.values() for index in indices)
        same_batch_order = (
            not unmatched
            and not unmatched_target
            and [item["target_flow"] for item in comparisons] == list(self.decisions)
        )
        window = self.window(operation_pairs)
        checks = [item["checks"] for item in comparisons]
        checks.extend(item["model_body"]["checks"] for item in comparisons if "model_body" in item)
        failures = any(False in item.values() for item in checks)

        # A complete window can prove cross-batch dependencies that a single batch cannot see.
        unverified = any(
            value is None and not (name == "execution_relations" and window["status"] == "matched")
            for item in checks
            for name, value in item.items()
        )
        unassociated = len(self.physical["unassociated_tasks"]) + len(self.physical["unassociated_runtime_actions"])
        status = "matched"
        if unmatched or unmatched_target or unassociated or unverified or not comparisons:
            status = "partial"
        if failures or not self.control_scope["closed"]:
            status = "differences"

        # Coverage gaps do not erase a difference already established between paired calls.
        if window["status"] == "differences" or (status == "matched" and window["status"] == "partial"):
            status = window["status"]
        return {
            "scope": (
                "rank-local serving window: all public operations, data/CPU return and execution dependencies; "
                "request/token model bodies and native result-state snapshots checked separately; "
                "batch and observed caller/serialized order reported separately; "
                "opaque geometry, captured inputs, policy equivalence, timing and distributed progress unqualified"
            ),
            "controller_scope": self.control_scope,
            "status": status,
            "rank": self.rank,
            "same_batch_order": same_batch_order,
            "window": window,
            "comparisons": comparisons,
            "unmatched_native_batches": unmatched,
            "unmatched_target_batches": unmatched_target,
            "unassociated_tasks": len(self.physical["unassociated_tasks"]),
            "unassociated_runtime_actions": len(self.physical["unassociated_runtime_actions"]),
        }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    arguments = parser.parse_args()
    comparison = json.loads(arguments.input.read_text())
    prediction = json.loads(Path(comparison["prediction"]).read_text())
    specification = json.loads(Path(comparison["input"]).read_text())

    # One immutable prediction and its indexes serve all rank-local comparisons.
    target = TargetOperations(prediction["graph"])
    communication = CommunicationComparison(
        target, comparison["ranks"], prediction["decisions"], prediction["configuration"]["page_size"],
    )
    observations = []
    failed = False
    for case in comparison["ranks"]:
        output = Path(case["output"])
        published = output.is_file()
        try:
            if published:
                report = json.loads(output.read_text())
            else:
                rank = StructureComparison(
                    Path(case["source"]), Path(case["physical"]), Path(case["observed"]),
                    prediction, specification, target, communication,
                )
                if rank.rank != case["rank"]:
                    raise ValueError("comparison source rank differs from its declared rank")
                report = rank.compare()
                write_json(output, report, indent=2)
            if report["status"] == "error":
                failed = True
            else:
                observations.append(report["window"]["communication"])
        except Exception as error:

            # A bad rank must not hide the remaining planned comparisons or its traceback.
            traceback.print_exc()
            report = {
                "rank": case["rank"],
                "status": "error",
                "error_type": type(error).__name__,
                "error": str(error),
            }
            failed = True

            # A bad published report remains evidence; only this attempt's new output may be replaced.
            if not published:
                write_json(output, report, indent=2)
        print(json.dumps({"rank": case["rank"], "status": report["status"]}), flush=True)
    write_json(Path(comparison["communication_output"]), communication.compare(observations), indent=2)
    if failed:
        raise SystemExit(2)
