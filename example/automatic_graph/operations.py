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
        self.native_producers = {value["id"]: value["producer"] for value in observed["values"]}
        self.target_producers = {value["id"]: value["producer"] for value in target["values"]}
        self.operation_paths = DependencyPaths(target)

        execution = target["execution"]
        self.execution_paths = DependencyPaths(execution)
        self.target_calls = {call["operation"]: call for call in execution["host_calls"] if "operation" in call}
        entries = {call["entry"] for call in self.target_calls.values()}
        resources = {node["id"]: node["resource"] for node in execution["nodes"] if node["id"] in entries}
        self.target_actors = {operation: resources[call["entry"]] for operation, call in self.target_calls.items()}

    @staticmethod
    def call_signature(operations: list[dict[str, Any]]) -> tuple[Any, ...]:

        # Output arity distinguishes call contracts; shapes and values remain independent checks.
        return tuple((operation["kind"], operation["name"], len(operation["outputs"])) for operation in operations)

    def input_producers(self, operation: dict[str, Any], observed: bool) -> dict[Any, int | None]:
        graph = self.source if observed else self.target_graph
        region = graph["regions"][operation["region"]]
        metadata = region["metadata"] if observed else region
        inputs = (
            metadata.get("ports", {}).get("inputs", {})
            if operation["kind"] == "interface"
            else dict(enumerate(operation["inputs"]))
        )
        producers = self.native_producers if observed else self.target_producers
        return {name: producers[value] for name, value in inputs.items()}

    def calls_by_inputs(
        self, operations: list[dict[str, Any]], observed: bool, known: dict[int, dict[str, Any]]
    ) -> dict[Any, list[dict[str, Any]]]:
        groups = defaultdict(list)
        owners = known.keys() if observed else {operation["id"] for operation in known.values()}
        for operation in operations:
            inputs = self.input_producers(operation, observed)
            if not inputs or any(owner not in owners for owner in inputs.values()):
                continue

            if observed:
                inputs = {name: known[owner]["id"] for name, owner in inputs.items()}
            signature = self.call_signature([operation])[0], tuple(sorted(inputs.items()))
            groups[signature].append(operation)
        return groups

    def correspondence(
        self,
        native: list[dict[str, Any]],
        target: list[dict[str, Any]],
        known: dict[int, dict[str, Any]] | None = None,
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

        # Concurrent actors may interleave differently; known calls identify their actor and sequence boundaries.
        sequences = defaultdict(list)
        for actor, operations in native_groups.items():
            signature = self.call_signature(operations)
            sequences[signature].append(actor)
        candidates = defaultdict(list)
        for actor, operations in target_groups.items():
            signature = self.call_signature(operations)
            candidates[signature].append(actor)
        paired = dict(known or {})
        paired_targets = {operation["id"] for operation in paired.values()}
        conflicts = 0
        unresolved = []
        bound_actors = set()
        actor_pairs = []
        for operations in native_groups.values():
            signature = self.call_signature(operations)
            bound = {
                self.target_actors[paired[operation["id"]]["id"]]
                for operation in operations
                if operation["id"] in paired
            }
            matches = bound or (set(candidates[signature]) if len(sequences[signature]) == 1 else set())
            if len(matches) != 1:
                conflicts += len(bound) > 1
                unresolved.append({"native_operations": len(operations), "target_candidates": len(matches)})
                continue
            (actor,) = matches
            if actor in bound_actors:
                conflicts += 1
                continue
            bound_actors.add(actor)
            matched = target_groups.get(actor, [])
            positions = {operation["id"]: index for index, operation in enumerate(matched)}
            if any(
                paired[operation["id"]]["id"] not in positions
                for operation in operations if operation["id"] in paired
            ):
                conflicts += 1
                continue
            actor_pairs.append((operations, matched))
            anchors = [
                (index, positions[paired[operation["id"]]["id"]])
                for index, operation in enumerate(operations)
                if operation["id"] in paired
            ]
            boundaries = [(-1, -1), *anchors, (len(operations), len(matched))]
            expected = self.call_signature(matched)

            # Match only complete equal intervals; extra calls remain unpaired rather than shifting later identities.
            for begin, end in pairwise(boundaries):
                left = slice(begin[0] + 1, end[0])
                right = slice(begin[1] + 1, end[1])
                occupied = any(operation["id"] in paired_targets for operation in matched[right])
                if begin[1] >= end[1] or occupied or signature[left] != expected[right]:
                    unresolved.append(
                        {"native_operations": len(signature[left]), "target_operations": len(expected[right])}
                    )
                    continue
                paired.update(
                    (source["id"], target) for source, target in zip(operations[left], matched[right], strict=True)
                )
                paired_targets.update(operation["id"] for operation in matched[right])

        # Equal call sequences can act on different retained batches; known input owners must also agree.
        ambiguous = []
        for operation in native:
            identity = operation["id"]
            if identity not in paired:
                continue
            left = self.input_producers(operation, True)
            right = self.input_producers(paired[identity], False)
            if any(
                left[name] in paired and right[name] is not None and paired[left[name]]["id"] != right[name]
                for name in left.keys() & right.keys()
            ):
                ambiguous.append(identity)
        for identity in ambiguous:
            if known is not None and identity in known:
                conflicts += 1
            else:
                del paired[identity]

        # Use accepted input owners only after rejecting sequence conflicts; repeated calls remain ambiguous.
        paired_targets = {operation["id"] for operation in paired.values()}
        for operations, matched in actor_pairs:
            if all(operation["id"] in paired for operation in operations):
                continue
            native_inputs = self.calls_by_inputs(operations, True, paired)
            target_inputs = self.calls_by_inputs(matched, False, paired)
            for key in native_inputs.keys() & target_inputs.keys():
                if len(native_inputs[key]) != 1 or len(target_inputs[key]) != 1:
                    continue
                source, predicted = native_inputs[key][0], target_inputs[key][0]
                if source["id"] not in paired and predicted["id"] not in paired_targets:
                    paired[source["id"]] = predicted
                    paired_targets.add(predicted["id"])

        # Record observed caller order separately; independent requests can change order with completion timing.
        required = {
            (self.target_calls[left["id"]]["return"], self.target_calls[right["id"]]["entry"])
            for operations in native_groups.values()
            for left, right in pairwise(paired[item["id"]] for item in operations if item["id"] in paired)
        }
        missing = self.execution_paths.missing(required)
        target_ids = {item["id"] for item in paired.values()}
        consistent = not conflicts and len(target_ids) == len(paired)
        complete = consistent and len(paired) == len(native) == len(target)
        return paired, {
            "complete": complete,
            "consistent_bindings": consistent,
            "conflicting_bindings": conflicts,
            "ambiguous_input_owners": len(ambiguous),
            "input_owner_examples": ambiguous[:3],
            "native_actors": len(native_groups),
            "target_actors": len(target_groups),
            "paired_operations": len(paired),
            "unmatched_native_operations": len(native) - len(paired),
            "unmatched_target_operations": len(target) - len(target_ids),
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
        missing = self.operation_paths.missing(required)

        # A logical dependency also requires its producer's CPU return before the consuming call enters.
        calls = {
            (self.target_calls[source]["return"], self.target_calls[target]["entry"]): (source, target)
            for source, target in required
        }
        missing.update(calls[link] for link in self.execution_paths.missing(set(calls)))
        return sorted((positions[source], positions[target]) for source, target in missing)

