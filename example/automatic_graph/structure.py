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
        self.native_batches = self.region_batches(self.source["regions"], True)
        self.target_batches = self.region_batches(self.prediction["graph"]["regions"], False)

    @staticmethod
    def region_batches(regions: list[dict[str, Any]], observed: bool) -> list[int | None]:
        result = []
        for region in regions:
            batch = (region["metadata"] if observed else region).get("batch")
            if batch is None and region["parent"] is not None:
                batch = result[region["parent"]]
            result.append(batch)
        return result

    def signature(self, batch: dict[str, Any], observed: bool) -> tuple[Any, ...]:
        members = []
        for member in batch["members"]:
            identity = self.request_ids[member["id"]] if observed else member["id"]
            members.append((identity, member["new_tokens"], member["context_tokens"]))
        return batch["phase"], tuple(members)

    def batch_operations(
        self, graph: dict[str, Any], batches: list[int | None], batch: int, observed: bool
    ) -> list[dict[str, Any]]:
        return [
            operation
            for operation in graph["operations"]
            if batches[operation["region"]] == batch
            and (observed or graph["regions"][operation["region"]]["rank"] == self.rank)
        ]

    def operation_rows(self, graph: dict[str, Any], selected: list[dict[str, Any]], observed: bool) -> tuple[Any, Any]:
        values = self.source["values"] if observed else graph["values"]
        rows = []
        for operation in selected:
            row = [operation["name"]]

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

        # Local operation positions remove observation IDs while retaining actual dependency direction and kind.
        positions = {operation["id"]: index for index, operation in enumerate(selected)}
        edges = {
            (positions[edge["source"]], positions[edge["target"]], edge["kind"])
            for edge in graph["dependencies"]
            if edge["source"] in positions and edge["target"] in positions
        }
        return rows, sorted(edges)

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

    def native_batch(self, record: dict[str, Any]) -> int | None:
        region = record["region"]
        if region is None and record["operation"] is not None:
            region = self.source["operations"][record["operation"]]["region"]
        return self.native_batches[region] if region is not None else None

    def native_work(self) -> dict[int, Counter[str]]:
        result: dict[int, Counter[str]] = defaultdict(Counter)
        for work in self.physical["work"]:
            if work["observer"] or work["auxiliary"] or work["baseline"] is not None:
                continue
            if work["kind"] != "device" or work["task_type"] in ("EVENT_RECORD", "EVENT_WAIT"):
                continue
            operation = work["operation"]
            if operation is not None and self.source["operations"][operation].get("communication", False):
                continue
            batch = self.native_batch(work)
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
            batch = self.native_batch(action)
            if batch is not None:
                result[batch]["copy_async:" + action["native_api"]] += 1
        return result

    def target_work(self) -> dict[int, Counter[str]]:
        graph = self.prediction["graph"]
        result: dict[int, Counter[str]] = defaultdict(Counter)
        for node in graph["execution"]["nodes"]:
            if node["kind"] not in ("device", "copy_async"):
                continue
            region = node.get("region")
            if region is None:
                region = graph["operations"][node["operation"]]["region"]
            if graph["regions"][region]["rank"] == self.rank:
                name = node["name"] if node["kind"] == "device" else "copy_async:" + node["name"]
                result[self.target_batches[region]][name] += 1
        return result

    @staticmethod
    def missing_orders(observed: list[Any], predicted: list[Any]) -> list[tuple[int, int]]:
        successors: dict[int, set[int]] = defaultdict(set)
        for source, target, _ in predicted:
            successors[source].add(target)
        required: dict[int, set[int]] = defaultdict(set)
        for source, target, _ in observed:
            required[source].add(target)

        missing = []
        for source, pending in required.items():
            visited = set()
            stack = list(successors[source])
            while stack and pending:
                target = stack.pop()
                if target in visited:
                    continue
                visited.add(target)
                pending.discard(target)
                stack.extend(successors[target] - visited)
            missing.extend((source, target) for target in sorted(pending))
        return missing

    def state(self, batch: dict[str, Any], decision: dict[str, Any]) -> dict[str, bool]:
        regions = [region for region in self.source["regions"] if self.native_batches[region["id"]] == batch["id"]]
        (result,) = [region for region in regions if region["role"] == "ResultProcessed"]
        values = self.source["values"]
        ports = result["metadata"]["ports"]
        native_effects = [
            (self.request_ids[item["id"]], item["generated_tokens"], item["finished"])
            for item in result["metadata"]["effects"]
        ]
        target_effects = [(item["id"], item["generated_tokens"], item["finished"]) for item in decision["members"]]
        released = [
            region["metadata"].get("observed_result")
            for region in regions
            if region["metadata"].get("interface") == "tensor_unique"
        ]
        expected = [
            sorted(page + 1 for page in item["released_pages"]) for item in decision["members"] if item["finished"]
        ]
        if batch["phase"] == "decode" and expected:
            expected = [sorted(page for pages in expected for page in pages)]

        return {
            "slots": [item["slot"] for item in batch["members"]] == [item["slot"] + 1 for item in decision["members"]],
            "effects": native_effects == target_effects,
            "free_pages_before_result": values[ports["inputs"]["free_pages"]]["shape"][0]
            == decision["free_pages_before"] - decision["allocated_pages"],
            "free_pages_after_result": values[ports["outputs"]["free_pages"]]["shape"][0]
            == decision["free_pages_after"],
            "released_page_ids": released == expected,
        }

    def window(self) -> dict[str, Any]:
        model_batches = {
            self.native_batches[region["id"]]
            for region in self.source["regions"]
            if region["role"] == "ResultProcessed"
        }
        first, last = min(model_batches), max(model_batches)
        native = [
            operation
            for operation in self.observed["operations"]
            if (batch := self.native_batches[operation["region"]]) is not None and first <= batch <= last
        ]
        graph = self.prediction["graph"]
        target = [
            operation for operation in graph["operations"] if graph["regions"][operation["region"]]["rank"] == self.rank
        ]
        left, left_edges = self.operation_rows(self.observed, native, True)
        right, right_edges = self.operation_rows(graph, target, False)
        same_calls = [row[0] for row in left] == [row[0] for row in right]
        report = {
            "scope": (
                "first scheduled model batch through final ResultProcessed, including intervening control; "
                "terminal polling excluded"
            ),
            "native_operations": len(native),
            "target_operations": len(target),
            "same_calls": same_calls,
            "same_operation_geometry": left == right,
            "status": "differences" if left != right else "matched",
        }
        if same_calls:
            missing = self.missing_orders(left_edges, right_edges)
            report.update(
                interface_ports=self.interface_ports(native, target),
                native_dependency_order=not missing,
                missing_dependency_orders=missing,
                execution=ExecutionComparison(native, target, graph["execution"]).compare(),
            )
            if not report["interface_ports"]["same"] or missing or report["execution"]["same"] is False:
                report["status"] = "differences"
            elif report["status"] == "matched" and report["execution"]["same"] is None:
                report["status"] = "partial"
        return report

    def compare(self) -> dict[str, Any]:
        target: dict[tuple[Any, ...], deque[int]] = defaultdict(deque)
        decisions = self.prediction["decisions"]
        for index, decision in enumerate(decisions):
            target[self.signature(decision, False)].append(index)
        native_work = self.native_work()
        target_work = self.target_work()
        comparisons = []
        unmatched = []
        serving_batches = {
            self.native_batches[region["id"]]
            for region in self.source["regions"]
            if region["role"] == "ResultProcessed"
        }

        for batch in self.source["batches"]:
            if batch["observation"] != "source" or batch["id"] not in serving_batches:
                continue
            key = self.signature(batch, True)
            if not target[key]:
                unmatched.append(batch["id"])
                continue
            index = target[key].popleft()
            native = self.batch_operations(self.observed, self.native_batches, batch["id"], True)
            predicted = self.batch_operations(self.prediction["graph"], self.target_batches, index, False)
            native_rows, native_edges = self.operation_rows(self.observed, native, True)
            target_rows, target_edges = self.operation_rows(self.prediction["graph"], predicted, False)
            same_rows = native_rows == target_rows
            same_calls = [row[0] for row in native_rows] == [row[0] for row in target_rows]
            ports = self.interface_ports(native, predicted) if same_calls else None
            missing_orders = self.missing_orders(native_edges, target_edges) if same_calls else None
            work = native_work[batch["id"]]
            predicted_work = target_work[index]
            checks = self.state(batch, decisions[index])
            checks.update(
                operations=same_rows,
                native_dependency_order=not missing_orders if same_calls else None,
                compute_and_async_copies=work == predicted_work,
                public_interface_ports=ports["same"] if ports is not None else None,
            )
            comparison = {"native_batch": batch["id"], "target_batch": index, "checks": checks}
            if ports is not None:
                comparison["interface_ports"] = ports
            if missing_orders:
                comparison["missing_dependency_orders"] = missing_orders
            if not same_rows:
                first = next(
                    (
                        position
                        for position, pair in enumerate(zip(native_rows, target_rows, strict=False))
                        if pair[0] != pair[1]
                    ),
                    min(len(native_rows), len(target_rows)),
                )
                comparison["operation_difference"] = {
                    "counts": [len(native_rows), len(target_rows)],
                    "position": first,
                    "native": native_rows[first : first + 1],
                    "target": target_rows[first : first + 1],
                }
            if work != predicted_work:
                comparison["missing_work"] = dict(work - predicted_work)
                comparison["extra_work"] = dict(predicted_work - work)
            comparisons.append(comparison)

        unmatched_target = sorted(index for indices in target.values() for index in indices)
        same_batch_order = (
            not unmatched
            and not unmatched_target
            and [item["target_batch"] for item in comparisons] == list(range(len(decisions)))
        )
        failures = sum(any(value is False for value in item["checks"].values()) for item in comparisons)
        unverified = any(value is None for item in comparisons for value in item["checks"].values())
        unassociated = len(self.physical["unassociated_tasks"]) + len(self.physical["unassociated_runtime_actions"])
        status = "matched"
        if unmatched or unmatched_target or unassociated or unverified or not comparisons:
            status = "partial"
        if failures or (not unmatched and not unmatched_target and not same_batch_order):
            status = "differences"
        window = self.window()

        # A fully paired run also has to preserve the whole window's cross-call effects.
        if not unmatched and not unmatched_target and status != "differences":
            if window["status"] == "differences" or (status == "matched" and window["status"] == "partial"):
                status = window["status"]
        return {
            "scope": (
                "matched native batches: public geometry/native dependency order, CPU state, compute and async APIs; "
                "named interface ports and ordered returns checked; "
                "opaque geometry, captured inputs, timing and distributed progress unqualified"
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
