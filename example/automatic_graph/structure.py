#!/usr/bin/env python3
"""Compare a prediction with native batch, state and device evidence without changing either graph."""

from __future__ import annotations

import argparse
import json
from collections import Counter, defaultdict, deque
from pathlib import Path
from typing import Any

from execution import ExecutionComparison
from markov_collection.artifacts.json_file import write_json


class StructureComparison:
    def __init__(self, source: Path, physical: Path, observed: Path, prediction: Path, input_path: Path) -> None:
        self.source = json.loads(source.read_text())
        self.physical = json.loads(physical.read_text())
        self.observed = json.loads(observed.read_text())
        self.prediction = json.loads(prediction.read_text())
        specification = json.loads(input_path.read_text())
        session = json.loads((source.parent / "session.json").read_text())
        self.request_ids = dict(
            zip(session["source_requests"], (item["id"] for item in specification["requests"]), strict=True)
        )
        self.rank = self.source["environment"]["rank"]
        self.stage = self.source["environment"]["pp_rank"]
        mode = self.source["environment"]["implementation"]["disaggregation_mode"]
        self.instance = "model" if mode == "null" else mode
        self.native_batches = self.region_labels(self.source["regions"], True, "batch")
        self.target_batches = self.region_labels(self.prediction["graph"]["regions"], False, "batch")
        self.target_actions = self.region_labels(self.prediction["graph"]["regions"], False, "action")
        self.native_requests = self.region_labels(self.source["regions"], True, "request")
        self.native_model = self.model_regions(self.source["regions"])
        self.target_model = self.model_regions(self.prediction["graph"]["regions"])

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
        self.flow_actions: dict[int, set[int]] = defaultdict(set)
        self.target_successors: dict[int, set[int]] = defaultdict(set)
        for region in self.prediction["graph"]["regions"]:
            flow = self.target_batches[region["id"]]
            action = self.target_actions[region["id"]]
            if region.get("rank") == self.rank and flow is not None and action is not None:
                self.flow_actions[flow].add(action)
        for edge in self.prediction["graph"]["dependencies"]:
            self.target_successors[edge["source"]].add(edge["target"])

        # A flow can span pipeline actions; PD instances share its identity but own different work.
        for decision in self.prediction["decisions"]:
            if decision["task"]["group"] != self.instance:
                continue
            flow = decision["flow"]
            if decision["commits_requests"] or decision["step"] == "model":
                self.completions[flow] = decision
            if decision["step"] in {"batch", "model", "accept"} or (
                decision["step"] == "stage" and decision["stage"] == self.stage
            ):
                self.decisions[flow] = decision

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
            and (
                observed
                or (
                    graph["regions"][operation["region"]].get("rank") == self.rank
                    and self.target_actions[operation["region"]] in self.flow_actions[batch]
                )
            )
        ]

    def operation_rows(self, graph: dict[str, Any], selected: list[dict[str, Any]], observed: bool) -> list[Any]:
        values = self.source["values"] if observed else graph["values"]
        rows = []
        for operation in selected:
            communication = operation.get("communication")
            if observed and isinstance(communication, dict):
                communication = dict(communication)

                # Resolve recorded split counts in their native scope, without using target routing rules.
                for field in ("input_splits", "output_splits"):
                    if field in communication:
                        communication[field] = [
                            self.native_parameter(operation["region"], item["scalar"])
                            if isinstance(item, dict)
                            else item
                            for item in communication[field]
                        ]
            row = [operation["name"], communication]

            # Native interfaces may expose opaque storage; their internal geometry is not a public tensor contract.
            if operation["kind"] == "interface":
                rows.append(row)
                continue
            for direction in ("inputs", "outputs"):
                row.append(
                    [
                        {key: values[identity].get(key) for key in ("shape", "dtype", "device")}
                        for identity in operation[direction]
                    ]
                )
            rows.append(row)
        return rows

    def native_parameter(self, region: int, name: str) -> Any:
        while region is not None:
            scope = self.source["regions"][region]
            parameters = scope["metadata"].get("parameters", {})
            if name in parameters:
                return parameters[name]
            region = scope["parent"]
        raise KeyError(f"native communication parameter is not recorded: {name}")

    @staticmethod
    def operation_edges(graph: dict[str, Any], selected: list[dict[str, Any]]) -> list[Any]:

        # Local operation positions remove observation IDs while retaining actual dependency direction and kind.
        positions = {operation["id"]: index for index, operation in enumerate(selected)}
        edges = {
            (positions[edge["source"]], positions[edge["target"]], edge["kind"])
            for edge in graph["dependencies"]
            if edge["source"] in positions and edge["target"] in positions
        }
        return sorted(edges)

    @staticmethod
    def operation_difference(native: list[Any], target: list[Any]) -> dict[str, Any]:
        first = next(
            (position for position, pair in enumerate(zip(native, target, strict=False)) if pair[0] != pair[1]),
            min(len(native), len(target)),
        )
        return {
            "counts": [len(native), len(target)],
            "position": first,
            "native": native[first : first + 1],
            "target": target[first : first + 1],
        }

    def compare_operations(self, native: list[dict[str, Any]], target: list[dict[str, Any]]) -> dict[str, Any]:
        left = self.operation_rows(self.observed, native, True)
        right = self.operation_rows(self.prediction["graph"], target, False)
        same_calls = [row[0] for row in left] == [row[0] for row in right]
        ports = self.interface_ports(native, target) if same_calls else None
        missing = self.missing_orders(self.operation_edges(self.observed, native), target) if same_calls else None
        report: dict[str, Any] = {
            "same_calls": same_calls,
            "checks": {
                "operations": left == right,
                "native_dependency_order": not missing if same_calls else None,
                "public_interface_ports": ports["same"] if ports is not None else None,
            },
        }
        if ports is not None:
            report["interface_ports"] = ports
        if missing:
            report["missing_dependency_orders"] = missing
        if left != right:
            report["operation_difference"] = self.operation_difference(left, right)
        return report

    def interface_ports(self, native: list[dict[str, Any]], target: list[dict[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {
            "same": True,
            "checked_ports": 0,
            "opaque_geometry_ports": 0,
            "interfaces_without_named_ports": 0,
        }
        for position, (left, right) in enumerate(zip(native, target, strict=True)):
            if left["kind"] != "interface":
                continue

            # Captured private resources have no positional API contract; only named ports identify the same operand.
            ports = self.source["regions"][left["region"]]["metadata"].get("ports")
            if ports is None:
                result["interfaces_without_named_ports"] += 1

                # Unnamed interface returns retain their declared order; captured inputs do not.
                ports = {"outputs": dict(enumerate(left["outputs"]))}
                expected_ports = {"outputs": dict(enumerate(right["outputs"]))}
            else:
                expected_ports = self.prediction["graph"]["regions"][right["region"]]["ports"]
            for direction in ports:
                observed = {}
                predicted = {}
                for name, source in ports[direction].items():
                    if name not in expected_ports[direction]:
                        continue
                    value = expected_ports[direction][name]
                    actual = self.source["values"][source]
                    expected = self.prediction["graph"]["values"][value]

                    # Opaque resources still expose dtype/device; only declared target tensors promise geometry.
                    fields = ("dtype", "device", "shape") if "shape" in expected else ("dtype", "device")
                    observed[name] = {key: actual.get(key) for key in fields}
                    predicted[name] = {key: expected.get(key) for key in fields}
                    result["checked_ports"] += 1
                    result["opaque_geometry_ports"] += "shape" not in expected

                if ports[direction].keys() != expected_ports[direction].keys() or observed != predicted:
                    result["same"] = False
                    if "first_difference" not in result:
                        result["first_difference"] = {
                            "operation": position,
                            "name": left["name"],
                            "direction": direction,
                            "port_names": [sorted(ports[direction]), sorted(expected_ports[direction])],
                            "native": observed,
                            "target": predicted,
                        }
        return result

    def native_batch(self, record: dict[str, Any], model_only: bool) -> int | None:
        region = record["region"]
        if region is None and record["operation"] is not None:
            region = self.source["operations"][record["operation"]]["region"]
        if region is None or (model_only and not self.native_model[region]):
            return None
        return self.native_batches[region]

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

    def missing_orders(self, observed: list[Any], predicted: list[dict[str, Any]]) -> list[tuple[int, int]]:
        required: dict[int, set[int]] = defaultdict(set)
        for source, target, _ in observed:
            required[source].add(target)

        missing = []

        # A local dependency may pass through another rank or flow before reaching its consumer.
        for source, targets in required.items():
            pending = {predicted[position]["id"]: position for position in targets}
            successors = self.target_successors[predicted[source]["id"]]
            for target in successors:
                pending.pop(target, None)
            visited = set()
            frontier = deque(successors)
            while frontier and pending:
                target = frontier.popleft()
                if target in visited:
                    continue
                visited.add(target)
                pending.pop(target, None)
                frontier.extend(self.target_successors[target] - visited)
            missing.extend((source, target) for target in sorted(pending.values()))
        return missing

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
            if ((batch := self.native_batches[operation["region"]]) is not None and first <= batch <= last)
            or self.native_requests[operation["region"]] in self.request_ids
        ]
        graph = self.prediction["graph"]
        target = [
            operation
            for operation in graph["operations"]
            if graph["regions"][operation["region"]].get("rank") == self.rank
        ]
        paired = [operation_pairs[operation["id"]] for operation in native if operation["id"] in operation_pairs]
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
        if not complete:
            return {**window, "status": "partial", "reason": "matched flows do not cover the entire window"}

        # Align logical owners only; dependency and execution checks still use the unchanged target graph.
        report = self.compare_operations(native, paired)
        report["execution"] = ExecutionComparison(native, paired, graph["execution"]).compare()
        report["checks"]["execution_relations"] = report["execution"]["same"]
        checks = report["checks"]
        same_order = [operation["id"] for operation in paired] == [operation["id"] for operation in target]
        return {
            **window,
            **report,
            "same_operation_order": same_order,
            "status": "differences" if False in checks.values() else "partial" if None in checks.values() else "matched",
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
            comparison = self.compare_operations(native, predicted)
            work = native_work[batch["id"]]
            predicted_work = target_work[index]
            comparison["checks"].update(self.state(batch, index))
            self.compare_work(comparison, work, predicted_work)
            comparison.update(native_batch=batch["id"], target_flow=index)
            if comparison["same_calls"]:
                operation_pairs.update((left["id"], right) for left, right in zip(native, predicted, strict=True))
                comparison["execution"] = ExecutionComparison(
                    native, predicted, self.prediction["graph"]["execution"]
                ).compare()
                comparison["checks"]["execution_relations"] = comparison["execution"]["same"]

            # Model reuse is checked separately; different control paths do not certify or refute its body.
            if self.decisions[index]["step"] != "accept":
                model_native = [operation for operation in native if self.native_model[operation["region"]]]
                model_target = [operation for operation in predicted if self.target_model[operation["region"]]]
                comparison["model_body"] = {
                    **self.compare_operations(model_native, model_target),
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
        if failures:
            status = "differences"
        window = self.window(operation_pairs)

        # A fully paired run also has to preserve the whole window's cross-call effects.
        if not unmatched and not unmatched_target and status != "differences":
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
