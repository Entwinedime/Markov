"""Compare fixed operation graphs independently of serving requests and scheduling."""

from __future__ import annotations

from collections import defaultdict
from itertools import pairwise
from typing import Any

from execution import DependencyPaths


class OperationComparison:
    def __init__(self, source: dict[str, Any], observed: dict[str, Any], target: dict[str, Any]) -> None:
        self.source = source
        self.observed = observed
        self.target_graph = target
        self.operation_paths = DependencyPaths(target)

        execution = target["execution"]
        self.execution_paths = DependencyPaths(execution)
        self.target_calls = {call["operation"]: call for call in execution["host_calls"] if "operation" in call}
        entries = {call["entry"] for call in self.target_calls.values()}
        resources = {node["id"]: node["resource"] for node in execution["nodes"] if node["id"] in entries}
        self.target_actors = {operation: resources[call["entry"]] for operation, call in self.target_calls.items()}

    def correspondence(
        self, native: list[dict[str, Any]], target: list[dict[str, Any]]
    ) -> tuple[dict[int, dict[str, Any]], dict[str, Any]]:
        native_groups = defaultdict(list)
        target_groups = defaultdict(list)
        for operation in native:

            # Opaque native services declare their own resource, independently of the calling CPU thread.
            service = operation.get("native_work")
            actor = (
                ("service", service["resource"])
                if service is not None
                else ("thread", self.source["regions"][operation["region"]]["thread"])
            )
            native_groups[actor].append(operation)
        for operation in target:
            target_groups[self.target_actors[operation["id"]]].append(operation)

        # Concurrent actors may interleave differently; repeated calls retain their actor-local occurrence.
        sequences = defaultdict(list)
        for operations in native_groups.values():
            signature = tuple((operation["kind"], operation["name"]) for operation in operations)
            sequences[signature].append(operations)
        candidates = defaultdict(list)
        for operations in target_groups.values():
            signature = tuple((operation["kind"], operation["name"]) for operation in operations)
            candidates[signature].append(operations)
        paired = {}
        required = set()
        unresolved = []
        for signature, groups in sequences.items():
            matches = candidates.get(signature, [])
            if len(groups) != 1 or len(matches) != 1:
                unresolved.append({"native_actors": len(groups), "target_candidates": len(matches)})
                continue
            (operations,) = groups
            (matched,) = matches
            paired.update((left["id"], right) for left, right in zip(operations, matched, strict=True))
            required.update(
                (self.target_calls[left["id"]]["return"], self.target_calls[right["id"]]["entry"])
                for left, right in pairwise(matched)
            )

        # Matching a sequence does not prove that the target enforces the declared caller/service order.
        missing = self.execution_paths.missing(required)
        complete = len(paired) == len(native) == len(target)
        return paired, {
            "complete": complete,
            "native_actors": len(native_groups),
            "target_actors": len(target_groups),
            "paired_operations": len(paired),
            "unmatched_native_operations": len(native) - len(paired),
            "unmatched_target_operations": len(target) - len(paired),
            "unresolved_sequences": len(unresolved),
            "unresolved_examples": unresolved[:3],
            "same_serialized_order": (
                [paired[operation["id"]]["id"] for operation in native] == [operation["id"] for operation in target]
                if complete
                else None
            ),
            "caller_order": not missing,
            "caller_completion_links": len(required),
            "missing_caller_links": len(missing),
            "missing_examples": sorted(missing)[:3],
        }

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

    def compare(self, native: list[dict[str, Any]], target: list[dict[str, Any]]) -> dict[str, Any]:
        left = self.operation_rows(self.observed, native, True)
        right = self.operation_rows(self.target_graph, target, False)
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
                expected_ports = self.target_graph["regions"][right["region"]]["ports"]
            for direction in ports:
                observed = {}
                predicted = {}
                for name, source in ports[direction].items():
                    if name not in expected_ports[direction]:
                        continue
                    value = expected_ports[direction][name]
                    actual = self.source["values"][source]
                    expected = self.target_graph["values"][value]

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

    def missing_orders(self, observed: list[Any], predicted: list[dict[str, Any]]) -> list[tuple[int, int]]:
        required = {(predicted[source]["id"], predicted[target]["id"]) for source, target, _ in observed}
        positions = {operation["id"]: index for index, operation in enumerate(predicted)}
        return sorted(
            (positions[source], positions[target]) for source, target in self.operation_paths.missing(required)
        )

