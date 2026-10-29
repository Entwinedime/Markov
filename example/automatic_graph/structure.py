#!/usr/bin/env python3
"""Compare a prediction with native batch, state and device evidence without changing either graph."""

from __future__ import annotations

import argparse
import json
from collections import Counter, defaultdict, deque
from itertools import chain
from pathlib import Path
from typing import Any

from execution import ExecutionComparison
from markov_collection.artifacts import write_json
from operations import OperationComparison


class StructureComparison:
    def __init__(self, source: Path, physical: Path, observed: Path, prediction: Path, input_path: Path) -> None:
        self.source = json.loads(source.read_text())
        self.physical = json.loads(physical.read_text())
        self.observed = json.loads(observed.read_text())
        self.prediction = json.loads(prediction.read_text())
        self.operations = OperationComparison(self.source, self.observed, self.prediction["graph"])
        specification = json.loads(input_path.read_text())
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
        self.native_model = self.model_regions(self.source["regions"])
        self.target_model = self.model_regions(self.prediction["graph"]["regions"])
        self.native_controls = self.region_labels(self.source["regions"], True, "control_only")
        self.excluded_control, self.control_scope = self.controller_scope()

        # KV ownership callbacks carry request IDs even when no native model batch is active.
        initial_batches = {
            self.native_batches[region["id"]]
            for region in self.source["regions"]
            if region["role"] in {"Prefill", "PrebuiltResult"}
        }
        handoffs = {
            member["id"]: batch["id"]
            for batch in self.source["batches"]
            if batch["id"] in initial_batches
            for member in batch["members"]
        }
        for index, request in enumerate(self.native_requests):
            if self.native_batches[index] is None and request in handoffs:
                self.native_batches[index] = handoffs[request]

        self.decisions: dict[int, dict[str, Any]] = {}
        self.completions = {}

        # A batch can span pipeline actions; P and D select their own member groups.
        groups = {
            group["id"] for group in self.prediction["graph"]["topology"]["groups"]
            if self.rank in group["members"]
        }
        for decision in self.prediction["decisions"]:
            if decision["task"]["group"] not in groups:
                continue
            flow = decision["flow"]
            if decision["commits_requests"] or decision["step"] == "model":
                self.completions[flow] = decision
            if decision["step"] in {"batch", "model", "compute", "accept"} or (
                decision["step"] == "stage" and decision["stage"] == self.stage
            ):
                self.decisions[flow] = decision

        # Request-owned transport attaches to that instance's initial batch on both sides.
        target_handoffs = {
            member["id"]: flow
            for flow, decision in self.decisions.items() if decision["phase"] == "prefill"
            for member in decision["members"]
        }
        actions = {(item["task"]["group"], item["task"]["sequence"]): item for item in self.prediction["decisions"]}
        self.flow_actions: dict[int, set[tuple[str, int]]] = defaultdict(set)
        for region in self.prediction["graph"]["regions"]:
            action = self.target_actions[region["id"]]
            if region.get("rank") != self.rank or action is None:
                continue
            decision = actions[action]
            if decision["step"] in {"reserve", "transfer", "release"}:
                (member,) = decision["members"]
                self.target_batches[region["id"]] = target_handoffs[member["id"]]
            flow = self.target_batches[region["id"]]
            if flow is not None:
                self.flow_actions[flow].add(action)

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
                region["role"] in {"Prefill", "Decode"} or (region["parent"] is not None and result[region["parent"]])
            )
        return result

    def signature(self, batch: dict[str, Any], observed: bool, prebuilt: bool = False) -> tuple[Any, ...]:
        members = []
        for member in batch["members"]:
            identity = self.request_ids[member["id"]] if observed else member["id"]
            members.append(identity if prebuilt else (identity, member["new_tokens"], member["context_tokens"]))
        return "prebuilt" if prebuilt else batch["phase"], tuple(members)

    def batch_operations(
        self, graph: dict[str, Any], batches: list[int | None], batch: int, observed: bool
    ) -> list[dict[str, Any]]:
        return [
            operation
            for operation in graph["operations"]
            if batches[operation["region"]] == batch
            and (not observed or operation["id"] not in self.excluded_control)
            and (
                observed
                or (
                    graph["regions"][operation["region"]].get("rank") == self.rank
                    and self.target_actions[operation["region"]] in self.flow_actions[batch]
                )
            )
        ]

    def native_region(self, record: dict[str, Any]) -> int | None:
        region = record["region"]
        if region is None and record["operation"] is not None:
            region = self.source["operations"][record["operation"]]["region"]
        return region

    def native_batch(self, record: dict[str, Any], model_only: bool) -> int | None:
        region = self.native_region(record)
        if region is None or (model_only and not self.native_model[region]):
            return None
        if self.control_scope["closed"] and self.native_controls[region]:
            return None
        return self.native_batches[region]

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

    def native_work(self, model_only: bool = False) -> dict[int, Counter[str]]:
        result: dict[int, Counter[str]] = defaultdict(Counter)
        for work in self.physical["work"]:
            if work["observer"] or work["auxiliary"] or work["baseline"] is not None:
                continue
            if work["kind"] != "device" or work["task_type"] in ("EVENT_RECORD", "EVENT_WAIT"):
                continue
            operation = work["operation"]
            if operation is not None and self.source["operations"][operation].get("communication", False):
                continue
            batch = self.native_batch(work, model_only)
            if batch is not None:
                result[batch][work["name"]] += 1

        # Transfers can lack a profiler task; their native async API remains the common observable boundary.
        for action in self.physical["runtime_actions"]:
            if (
                action["kind"] != "copy_async"
                or action["observer"]
                or action["auxiliary"]
                or action["baseline"] is not None
            ):
                continue
            batch = self.native_batch(action, model_only)
            if batch is not None:
                result[batch]["copy_async:" + action["native_api"]] += 1
        return result

    def target_work(self, model_only: bool = False) -> dict[int, Counter[str]]:
        graph = self.prediction["graph"]
        result: dict[int, Counter[str]] = defaultdict(Counter)
        for node in graph["execution"]["nodes"]:
            if node["kind"] not in ("device", "copy_async"):
                continue
            region = node.get("region")
            if region is None:
                region = graph["operations"][node["operation"]]["region"]
            batch = self.target_batches[region]
            if (
                graph["regions"][region].get("rank") == self.rank
                and self.target_actions[region] in self.flow_actions[batch]
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
        expected = (
            [sorted(page + 1 for page in item["released_pages"]) for item in completion["members"] if item["finished"]]
            if completion is not None
            else None
        )
        if batch["phase"] == "decode" and expected:
            expected = [sorted(page for pages in expected for page in pages)]

        checks = {
            "slots": [item["slot"] for item in batch["members"]] == [item["slot"] + 1 for item in decision["members"]],
            "effects": native_effects == target_effects if target_effects is not None else None,
            "released_page_ids": released == expected if expected is not None else None,
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
        model_batches = {
            self.native_batches[region["id"]]
            for region in self.source["regions"]
            if region["role"] in {"ResultProcessed", "PrebuiltResult"}
        }
        if not model_batches:
            return {"status": "partial", "reason": "no native model or prebuilt completion in the observation"}
        first, last = min(model_batches), max(model_batches)
        native = [
            operation
            for operation in self.observed["operations"]
            if operation["id"] not in self.excluded_control
            and (
                ((batch := self.native_batches[operation["region"]]) is not None and first <= batch <= last)
                or self.native_requests[operation["region"]] in self.request_ids
            )
        ]
        graph = self.prediction["graph"]
        target = [
            operation
            for operation in graph["operations"]
            if graph["regions"][operation["region"]].get("rank") == self.rank
        ]
        paired_native = [operation for operation in native if operation["id"] in operation_pairs]
        paired = [operation_pairs[operation["id"]] for operation in paired_native]
        paired_ids = {operation["id"] for operation in paired}
        target_ids = {operation["id"] for operation in target}
        complete = len(paired) == len(native) == len(target) and paired_ids == target_ids
        window = {
            "scope": (
                "rank-local model/prebuilt window and request-owned transport, including intervening control; "
                "terminal polling and other ranks excluded"
            ),
            "native_operations": len(native),
            "target_operations": len(target),
            "correspondence": {
                "complete": complete,
                "paired_native_operations": len(paired),
                "paired_target_operations": len(paired_ids),
            },
        }
        if not paired:
            return {**window, "status": "partial", "reason": "no paired operations in the window"}

        # Unpaired work limits coverage, but cannot hide dependencies between paired owners.
        report = self.operations.compare(paired_native, paired)
        report["execution"] = ExecutionComparison(paired_native, paired, graph["execution"]).compare()
        report["checks"]["execution_relations"] = report["execution"]["same"]
        checks = report["checks"]
        same_order = [operation["id"] for operation in paired] == [operation["id"] for operation in target]
        status = "matched" if complete and None not in checks.values() else "partial"
        if False in checks.values():
            status = "differences"
        return {
            **window,
            **report,
            "same_operation_order": same_order,
            "status": status,
        }

    def compare(self) -> dict[str, Any]:
        target: dict[tuple[Any, ...], deque[int]] = defaultdict(deque)
        for flow, decision in self.decisions.items():
            target[self.signature(decision, False, decision["step"] == "accept")].append(flow)
        native_work = self.native_work()
        target_work = self.target_work()
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
            if batch["observation"] != "source" or batch["id"] not in serving_batches:
                continue
            key = self.signature(batch, True, serving_batches[batch["id"]] == "PrebuiltResult")
            if not target[key]:
                unmatched.append(batch["id"])
                continue
            index = target[key].popleft()
            native = self.batch_operations(self.observed, self.native_batches, batch["id"], True)
            predicted = self.batch_operations(self.prediction["graph"], self.target_batches, index, False)
            paired, correspondence = self.operations.correspondence(native, predicted)
            if correspondence["complete"]:
                predicted = [paired[operation["id"]] for operation in native]
            comparison = self.operations.compare(native, predicted)
            comparison["correspondence"] = correspondence
            comparison["checks"]["call_correspondence"] = True if correspondence["complete"] else None
            comparison["checks"]["caller_order"] = correspondence["caller_order"]
            work = native_work[batch["id"]]
            predicted_work = target_work[index]
            comparison["checks"].update(self.state(batch, index))
            self.compare_work(comparison, work, predicted_work)
            comparison.update(native_batch=batch["id"], target_flow=index)
            if paired:
                operation_pairs.update(paired)
                known = [operation for operation in native if operation["id"] in paired]
                comparison["execution"] = ExecutionComparison(
                    known, [paired[operation["id"]] for operation in known], self.prediction["graph"]["execution"]
                ).compare()
                comparison["checks"]["execution_relations"] = comparison["execution"]["same"]

            # Model reuse is checked separately; different control paths do not certify or refute its body.
            if self.decisions[index]["step"] != "accept":
                model_native = [operation for operation in native if self.native_model[operation["region"]]]
                model_target = [operation for operation in predicted if self.target_model[operation["region"]]]
                comparison["model_body"] = {
                    **self.operations.compare(model_native, model_target),
                    "native_operations": len(model_native),
                    "target_operations": len(model_target),
                }
                self.compare_work(comparison["model_body"], native_model_work[batch["id"]], target_model_work[index])
            comparisons.append(comparison)

        unmatched_target = sorted(index for indices in target.values() for index in indices)
        same_batch_order = (
            not unmatched
            and not unmatched_target
            and [item["target_flow"] for item in comparisons] == list(self.decisions)
        )
        failures = sum(any(value is False for value in item["checks"].values()) for item in comparisons)
        unverified = any(value is None for item in comparisons for value in item["checks"].values())
        unassociated = len(self.physical["unassociated_tasks"]) + len(self.physical["unassociated_runtime_actions"])
        status = "matched"
        if unmatched or unmatched_target or unassociated or unverified or not comparisons:
            status = "partial"
        if failures or not self.control_scope["closed"]:
            status = "differences"
        window = self.window(operation_pairs)

        # Coverage gaps do not erase a difference already established between paired calls.
        if window["status"] == "differences" or (status == "matched" and window["status"] == "partial"):
            status = window["status"]
        return {
            "scope": (
                "rank-local matched flows: public geometry/native dependency order, available CPU state, "
                "compute and async APIs; "
                "named interface ports and ordered returns checked; "
                "batch and serialized operation order reported separately; "
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
    for name in ("source", "physical", "observed", "prediction", "input", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    arguments = parser.parse_args()
    report = StructureComparison(
        arguments.source, arguments.physical, arguments.observed, arguments.prediction, arguments.input
    ).compare()
    write_json(arguments.output, report, indent=2)
    print(json.dumps({"status": report["status"], "compared_batches": len(report["comparisons"])}))
