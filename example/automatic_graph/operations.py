"""Compare fixed operation graphs independently of serving requests and scheduling."""

from __future__ import annotations

import json
from collections import ChainMap, Counter, defaultdict
from collections.abc import Mapping, Set
from dataclasses import dataclass
from itertools import pairwise
from typing import Any

from execution import DependencyPaths


@dataclass
class RowAccess:
    reads: dict[str, int] | None
    writes: set[int] | None


class RowVersions:
    def __init__(
        self, graph: dict[str, Any], values: dict[int, dict[str, Any]], accesses: dict[tuple[int, int], RowAccess],
    ) -> None:
        self.operations = graph["operations"]
        self.values = values
        self.accesses = accesses

    def origins(self, operation: int, value: int) -> dict[str, int] | None:
        storage = self.values[value]["storage"]
        access = self.accesses.get((operation, storage))
        if access is None or access.reads is None:
            return None
        pending = set(access.reads.values())
        origins = {}

        # A whole-tensor version can include unrelated row writes; follow the actual selected rows.
        while pending:
            producer = self.values[value]["producer"]
            if producer is None:
                return None
            writer = self.operations[producer]
            if storage in writer["writes"]:
                previous = self.accesses.get((producer, storage))
                if previous is None or previous.writes is None:
                    return None
                for row in pending & previous.writes:
                    origins[row] = producer
                pending.difference_update(previous.writes)
            if pending:
                aliases = {identity for identity in writer["inputs"] if self.values[identity]["storage"] == storage}
                if len(aliases) != 1:
                    return None
                (value,) = aliases

        return {name: origins[row] for name, row in access.reads.items()}

    def descriptor(self, value: int) -> int | None:
        storage = self.values[value]["storage"]
        while (producer := self.values[value]["producer"]) is not None:
            access = self.accesses.get((producer, storage))

            # Only a proven destination-reference assignment keeps the existing tensor descriptor.
            if access is None or access.reads != {} or not access.writes:
                return producer
            aliases = {
                identity for identity in self.operations[producer]["inputs"]
                if self.values[identity]["storage"] == storage
            }
            if len(aliases) != 1:
                return producer
            (value,) = aliases
        return None

    def data_sources(self, operation: dict[str, Any]) -> set[int]:
        sources = set()
        for value in operation["inputs"]:
            origins = self.origins(operation["id"], value)
            if origins is not None:
                sources.update(origins.values())
                if (descriptor := self.descriptor(value)) is not None:
                    sources.add(descriptor)
            elif (producer := self.values[value]["producer"]) is not None:
                sources.add(producer)
        return sources

    def input_producer(self, operation: int, value: int) -> int | None:
        access = self.accesses.get((operation, self.values[value]["storage"]))

        # A destination reference needs its descriptor, not the last write to unrelated rows.
        return self.descriptor(value) if access is not None and access.reads == {} else self.values[value]["producer"]

    def disjoint(self, left: int, right: int, kind: str) -> bool:
        operands = {}
        for identity in (left, right):
            operation = self.operations[identity]
            operands[identity] = {
                "reads": {self.values[value]["storage"] for value in operation["inputs"]},
                "writes": set(operation["writes"]),
            }
        source, target = {
            "read_after_write": ("writes", "reads"),
            "write_after_read": ("reads", "writes"),
            "write_after_write": ("writes", "writes"),
        }[kind]
        shared = operands[left][source] & operands[right][target]
        if not shared:
            return False
        for storage in shared:
            before, after = self.accesses.get((left, storage)), self.accesses.get((right, storage))
            if before is None or after is None:
                return False
            first, second = getattr(before, source), getattr(after, target)
            if first is None or second is None:
                return False
            first = set(first.values()) if source == "reads" else first
            second = set(second.values()) if target == "reads" else second
            if first & second:
                return False
        return True


class TargetOperations:
    def __init__(self, graph: dict[str, Any]) -> None:
        self.graph = graph
        self.values = {value["id"]: value for value in graph["values"]}
        self.operation_paths = DependencyPaths(graph)

        execution = graph["execution"]
        self.execution_paths = DependencyPaths(execution)
        self.calls = {call["operation"]: call for call in execution["host_calls"] if "operation" in call}
        entries = {call["entry"] for call in self.calls.values()}
        resources = {}
        self.communication_ports: dict[int, dict[str, int]] = {}
        for node in execution["nodes"]:
            if node["id"] in entries:
                resources[node["id"]] = node["resource"]
            if node["name"] in {"collective_entry", "collective_exit"} and "operation" in node:
                port = "entry" if node["name"] == "collective_entry" else "return"
                self.communication_ports.setdefault(node["operation"], {})[port] = node["id"]
        self.actors = {operation: resources[call["entry"]] for operation, call in self.calls.items()}


class WorkOrigins:
    """Use recorded boundary matches to identify work across split calls and rank participants."""

    def __init__(self, bindings: list[dict], rank: int, regions: list[dict], target_tasks: list[Any]) -> None:
        links = defaultdict(set)
        for binding in bindings:
            task = ("task", binding["task"]["group"], binding["task"]["sequence"])
            for origin in binding["observations"]:
                if origin["rank"] != rank:
                    continue
                source = ("region", origin["region"])
                links[task].add(source)
                links[source].add(task)

        # One native call can have several boundaries; one transfer can include several native scopes.
        owners = {}
        for root in links:
            if root in owners:
                continue
            identity = len(owners)
            pending = [root]
            while pending:
                node = pending.pop()
                if node in owners:
                    continue
                owners[node] = identity
                pending.extend(links[node] - owners.keys())

        self.native = []
        for region in regions:
            parent = None if region["parent"] is None else self.native[region["parent"]]
            self.native.append(owners.get(("region", region["id"]), parent))
        self.target = [None if task is None else owners.get(("task", *task)) for task in target_tasks]

    def bind(
        self, native: list[dict], target: list[dict], comparison: OperationComparison, known: dict[int, dict],
    ) -> dict[str, Any]:
        groups = {"native": defaultdict(list), "target": defaultdict(list)}
        for side, operations, owners in (("native", native, self.native), ("target", target, self.target)):
            for operation in operations:
                groups[side][owners[operation["region"]]].append(operation)

        left, right = groups["native"], groups["target"]
        report = {"groups": len((left.keys() | right.keys()) - {None}), "conflicting_bindings": 0}
        occupied = {operation["id"] for operation in known.values()}
        for owner in dict.fromkeys((*left, *right)):
            if owner is None:
                continue
            target_ids = {operation["id"] for operation in right[owner]}
            anchors = {operation["id"]: known[operation["id"]] for operation in left[owner] if operation["id"] in known}
            if any(operation["id"] not in target_ids for operation in anchors.values()):
                report["conflicting_bindings"] += 1
                continue
            if len(anchors) == len(left[owner]) == len(right[owner]):
                continue

            # Unique labeled vertices or known inputs anchor this work graph; ordering is checked separately.
            candidates = []
            for operations, observed in ((left[owner], True), (right[owner], False)):
                signatures = defaultdict(list)
                for operation in operations:
                    signatures["call", comparison.call_signature([operation])[0]].append(operation)
                for signature, members in comparison.calls_by_inputs(operations, observed, known, occupied).items():
                    signatures["inputs", signature] = members
                candidates.append(signatures)
            native_inputs, target_inputs = candidates
            for key in native_inputs.keys() & target_inputs.keys():
                if len(native_inputs[key]) != 1 or len(target_inputs[key]) != 1:
                    continue
                source, predicted = native_inputs[key][0], target_inputs[key][0]
                if source["id"] not in known and predicted["id"] not in occupied:
                    anchors[source["id"]] = predicted
                    occupied.add(predicted["id"])
            pairs, bindings = comparison.correspondence(left[owner], right[owner], anchors, known, occupied)
            report["conflicting_bindings"] += bindings["conflicting_bindings"]
            known.update(pairs)
            occupied.update(operation["id"] for operation in pairs.values())

        report["unscoped_native_operations"] = len(left[None])
        report["unscoped_target_operations"] = len(right[None])
        report["unscoped_native_calls"] = dict(Counter(operation["name"] for operation in left[None]))
        report["unscoped_native_examples"] = [
            {key: operation[key] for key in ("id", "name", "region")} for operation in left[None][:3]
        ]
        return report

    def consistent(self, pairs: dict[int, dict], native: list[dict]) -> bool | None:
        covered = True
        for operation in native:
            if operation["id"] not in pairs:
                continue
            left = self.native[operation["region"]]
            right = self.target[pairs[operation["id"]]["region"]]
            if left is None or right is None:
                covered = False
            elif left != right:
                return False
        return True if covered else None

    def compatible(self, native: dict, target: dict) -> bool:
        left, right = self.native[native["region"]], self.target[target["region"]]
        return left is None or right is None or left == right


class OperationComparison:
    def __init__(
        self, source: dict[str, Any], observed: dict[str, Any], target: TargetOperations,
        origins: WorkOrigins,
        native_accesses: dict[tuple[int, int], RowAccess] | None = None,
        target_accesses: dict[tuple[int, int], RowAccess] | None = None,
    ) -> None:
        self.source = source
        self.observed = observed
        self.target = target
        self.origins = origins
        self.native_values = {value["id"]: value for value in observed["values"]}
        self.native_rows = RowVersions(observed, self.native_values, native_accesses or {})
        self.target_rows = RowVersions(target.graph, target.values, target_accesses or {})
        self.incoming = defaultdict(list)
        for edge in observed["dependencies"]:
            self.incoming[edge["target"]].append(edge)

    @staticmethod
    def call_signature(operations: list[dict[str, Any]]) -> tuple[Any, ...]:

        # State aliases can add outputs; named ports and tensor results are checked after calls are paired.
        return tuple((operation["kind"], operation["name"]) for operation in operations)

    def operand_values(self, operation: dict[str, Any], observed: bool, direction: str) -> dict[Any, int]:
        operands = operation[direction]
        if direction == "outputs":

            # Content aliases belong to storage effects, not positional tensor results.
            contents = set(operation.get("opaque_outputs", ()))
            operands = [value for value in operands if value not in contents]
        if operation["kind"] != "interface":
            return dict(enumerate(operands))

        graph = self.source if observed else self.target.graph
        region = graph["regions"][operation["region"]]
        metadata = region["metadata"] if observed else region
        ports = metadata.get("ports")
        if ports is not None and ports[direction]:
            return ports[direction]
        if direction == "outputs":
            return dict(enumerate(operands))

        # Argument paths identify the public ABI; opaque storage added to inputs has no positional contract.
        return self.argument_values({"arguments": operation["arguments"], "keywords": operation["keywords"]})

    @staticmethod
    def argument_values(argument: Any, path: str = "") -> dict[str, int]:
        if isinstance(argument, dict):
            if "value" in argument:
                return {path: argument["value"]}
            children = (
                ((json.dumps(key, sort_keys=True), value) for key, value in argument["mapping"])
                if "mapping" in argument else argument.items()
            )
        elif isinstance(argument, list):
            children = enumerate(argument)
        else:
            return {}

        result = {}
        for name, child in children:
            result.update(OperationComparison.argument_values(child, f"{path}/{name}"))
        return result

    def input_producers(self, operation: dict[str, Any], observed: bool) -> dict[Any, int | None]:
        rows = self.native_rows if observed else self.target_rows
        return {
            name: rows.input_producer(operation["id"], value)
            for name, value in self.operand_values(operation, observed, "inputs").items()
        }

    def same_input(
        self, native: dict[str, Any], target: dict[str, Any], name: Any, paired: Mapping[int, dict[str, Any]],
        target_owners: Mapping[int, list[int]],
    ) -> bool:
        original = self.operand_values(native, True, "inputs")[name]
        predicted = self.operand_values(target, False, "inputs")[name]
        descriptor = self.target_rows.descriptor(predicted)
        owners = () if descriptor is None else target_owners.get(descriptor, ())
        if len(owners) == 1:
            actual = self.native_values[original]
            fields = ("storage", "offset", "shape", "stride", "dtype", "device")
            native_outputs = self.operand_values(self.observed["operations"][owners[0]], True, "outputs")
            target_outputs = self.operand_values(self.target.graph["operations"][descriptor], False, "outputs")

            # Match the actual output port before comparing refreshed views; data ordering is checked separately.
            for port, value in target_outputs.items():
                if value != predicted or port not in native_outputs:
                    continue
                declared = self.native_values[native_outputs[port]]
                if all(actual[field] == declared[field] for field in fields):
                    return True

        left = self.native_rows.origins(native["id"], original)
        right = self.target_rows.origins(target["id"], predicted)

        # A write-only reference cannot prove which destination resource was selected.
        if not left or not right:
            return False
        before, after = self.native_rows.descriptor(original), self.target_rows.descriptor(predicted)
        same_descriptor = (
            (before is None and after is None)
            or (before in paired and paired[before]["id"] == after)
        )
        return (
            same_descriptor and left.keys() == right.keys()
            and all(owner in paired and paired[owner]["id"] == right[key] for key, owner in left.items())
        )

    def calls_by_inputs(
        self, operations: list[dict[str, Any]], observed: bool, known: Mapping[int, dict[str, Any]],
        targets: Set[int], context_targets: Set[int] = frozenset(),
    ) -> dict[Any, list[dict[str, Any]]]:
        groups = defaultdict(list)
        for operation in operations:
            inputs = self.input_producers(operation, observed)
            if not inputs:
                continue

            if observed:
                if any(owner not in known for owner in inputs.values()):
                    continue
                inputs = {name: known[owner]["id"] for name, owner in inputs.items()}
            elif any(owner not in targets and owner not in context_targets for owner in inputs.values()):
                continue
            signature = self.call_signature([operation])[0], tuple(sorted(inputs.items()))
            groups[signature].append(operation)
        return groups

    def native_groups(self, operations: list[dict[str, Any]]) -> dict[Any, list[dict[str, Any]]]:
        groups = defaultdict(list)
        for operation in operations:

            # Explicit services use the same resource identity as ordinary work on that thread or engine.
            service = operation.get("native_work")
            actor = (
                (service["resource"], service["identity"])
                if service is not None
                else ("thread", self.source["regions"][operation["region"]]["thread"])
            )
            groups[actor].append(operation)
        return groups

    def input_conflicts(
        self, native: list[dict], paired: dict[int, dict], context: dict[int, dict],
    ) -> tuple[list[int], int]:
        evidence = ChainMap(paired, context)
        checks = []
        for operation in native:
            identity = operation["id"]
            if identity not in paired:
                continue
            left = self.input_producers(operation, True)
            right = self.input_producers(paired[identity], False)
            mismatches = [
                name for name in left.keys() & right.keys()
                if left[name] in evidence and right[name] is not None and evidence[left[name]]["id"] != right[name]
            ]
            if mismatches:
                checks.append((operation, mismatches))

        # Index this evidence once; retain every owner so ambiguous bindings cannot become unique.
        target_owners = defaultdict(list)
        if checks:
            for identity, operation in evidence.items():
                target_owners[operation["id"]].append(identity)

        ambiguous = []
        verified = 0
        for operation, mismatches in checks:
            identity = operation["id"]
            resolved = sum(
                self.same_input(operation, paired[identity], name, evidence, target_owners) for name in mismatches
            )
            verified += resolved
            if resolved != len(mismatches):
                ambiguous.append(identity)
        return ambiguous, verified

    def correspondence(
        self,
        native: list[dict[str, Any]],
        target: list[dict[str, Any]],
        known: dict[int, dict[str, Any]] | None = None,
        context: dict[int, dict[str, Any]] | None = None,
        context_targets: Set[int] = frozenset(),
    ) -> tuple[dict[int, dict[str, Any]], dict[str, Any]]:
        native_groups = self.native_groups(native)
        target_groups = defaultdict(list)
        for operation in target:
            target_groups[self.target.actors[operation["id"]]].append(operation)

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
        context = context or {}
        paired_targets = {operation["id"] for operation in paired.values()}
        conflicts = 0
        unresolved = []
        bound_actors = set()
        actor_pairs = []
        for operations in native_groups.values():
            signature = self.call_signature(operations)
            bound = {
                self.target.actors[paired[operation["id"]]["id"]]
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
                same_work = all(
                    self.origins.compatible(source, target)
                    for source, target in zip(operations[left], matched[right], strict=False)
                )
                if begin[1] >= end[1] or occupied or signature[left] != expected[right] or not same_work:
                    unresolved.append(
                        {
                            "native_operations": len(signature[left]), "target_operations": len(expected[right]),
                            "calls": self.operation_difference(list(signature[left]), list(expected[right])),
                            "anchor_order": begin[1] < end[1],
                            "occupied_target": occupied,
                            "same_work": same_work,
                            "native_examples": [
                                {
                                    "id": operation["id"], "name": operation["name"],
                                    "work": self.origins.native[operation["region"]],
                                }
                                for operation in operations[left][:3]
                            ],
                            "target_examples": [
                                {
                                    "id": operation["id"], "name": operation["name"],
                                    "work": self.origins.target[operation["region"]],
                                }
                                for operation in matched[right][:3]
                            ],
                        }
                    )
                    continue
                paired.update(
                    (source["id"], target) for source, target in zip(operations[left], matched[right], strict=True)
                )
                paired_targets.update(operation["id"] for operation in matched[right])

        # Equal call sequences can act on different retained batches; known input owners must also agree.
        ambiguous, _ = self.input_conflicts(native, paired, context)
        rejected = set(ambiguous)
        for identity in rejected:
            if known is None or identity not in known:
                del paired[identity]

        # Advance the data frontier until stable; one pass cannot bind chains of newly identified producers.
        paired_targets = {operation["id"] for operation in paired.values()}
        while True:
            previous = len(paired)
            evidence = ChainMap(paired, context)
            for operations, matched in actor_pairs:
                if all(operation["id"] in paired for operation in operations):
                    continue
                native_inputs = self.calls_by_inputs(operations, True, evidence, paired_targets, context_targets)
                target_inputs = self.calls_by_inputs(matched, False, evidence, paired_targets, context_targets)
                for key in native_inputs.keys() & target_inputs.keys():
                    if len(native_inputs[key]) != 1 or len(target_inputs[key]) != 1:
                        continue
                    source, predicted = native_inputs[key][0], target_inputs[key][0]
                    if (
                        source["id"] not in paired and source["id"] not in rejected
                        and predicted["id"] not in paired_targets and self.origins.compatible(source, predicted)
                    ):
                        paired[source["id"]] = predicted
                        paired_targets.add(predicted["id"])
            if len(paired) == previous:
                break

        # New input evidence must also agree with earlier sequence candidates.
        ambiguous, storage_bindings = self.input_conflicts(native, paired, context)
        rejected.update(ambiguous)
        for identity in ambiguous:
            if known is None or identity not in known:
                del paired[identity]
        conflicts += sum(known is not None and identity in known for identity in rejected)

        target_ids = {item["id"] for item in paired.values()}
        consistent = not conflicts and len(target_ids) == len(paired)
        complete = consistent and len(paired) == len(native) == len(target)
        return paired, {
            "complete": complete,
            "consistent_bindings": consistent,
            "conflicting_bindings": conflicts,
            "ambiguous_input_owners": len(rejected),
            "verified_storage_bindings": storage_bindings,
            "input_owner_examples": sorted(rejected)[:3],
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
        }

    def caller_order(self, native: list[dict[str, Any]], paired: dict[int, dict[str, Any]]) -> dict[str, Any]:

        # Binding-only consumers do not need this report; independent requests may change caller order.
        required = {
            (self.target.calls[left["id"]]["return"], self.target.calls[right["id"]]["entry"])
            for operations in self.native_groups(native).values()
            for left, right in pairwise(paired[item["id"]] for item in operations if item["id"] in paired)
        }
        missing = self.target.execution_paths.missing(required)
        return {
            "caller_order": not missing,
            "caller_completion_links": len(required),
            "missing_caller_links": len(missing),
            "missing_examples": sorted(missing)[:3],
        }

    def operation_rows(
        self, graph: dict[str, Any], selected: list[dict[str, Any]], observed: bool, contracts: list[dict[str, Any]],
    ) -> list[Any]:
        values = self.source["values"] if observed else graph["values"]
        rows = []
        for operation, contract in zip(selected, contracts, strict=True):
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
                opaque = {
                    position for position, value in enumerate(contract[direction])
                    if value in contract.get("opaque_" + direction, ())
                }
                operands = []
                for position, identity in enumerate(operation[direction]):
                    if direction == "outputs" and identity in operation.get("opaque_outputs", ()):
                        continue

                    # Storage contents retain dtype/device; the declaring call does not promise a view's geometry.
                    fields = (
                        ("dtype", "device")
                        if direction == "inputs" and position in opaque
                        else ("shape", "dtype", "device")
                    )
                    operands.append({key: values[identity].get(key) for key in fields})
                row.append(operands)
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

    def operation_edges(self, selected: list[dict[str, Any]]) -> list[Any]:

        # Local operation positions remove observation IDs while retaining actual dependency direction and kind.
        positions = {operation["id"]: index for index, operation in enumerate(selected)}
        edges = set()
        for operation in selected:
            identity = operation["id"]
            sources = self.native_rows.data_sources(operation)
            for source in sources & positions.keys():
                edges.add((positions[source], positions[identity], "value"))
            for edge in self.incoming[identity]:
                source, kind = edge["source"], edge["kind"]
                if source not in positions or kind == "value":
                    continue

                # Only explicit, disjoint access ranges can remove conservative whole-storage hazards.
                if not self.native_rows.disjoint(source, identity, kind):
                    edges.add((positions[source], positions[identity], kind))
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
        left = self.operation_rows(self.observed, native, True, native)
        right = self.operation_rows(self.target.graph, target, False, native)
        same_calls = [row[0] for row in left] == [row[0] for row in right]
        ports = self.interface_ports(native, target) if same_calls else None
        missing = self.missing_orders(self.operation_edges(native), target) if same_calls else None
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

                # Public argument paths expose inputs without inventing names for captured private storage.
                ports = {direction: self.operand_values(left, True, direction) for direction in ("inputs", "outputs")}
                expected_ports = {
                    direction: self.operand_values(right, False, direction) for direction in ports
                }
            else:
                expected_ports = self.target.graph["regions"][right["region"]]["ports"]
            for direction in ports:
                observed = {}
                predicted = {}
                for name, source in ports[direction].items():
                    if name not in expected_ports[direction]:
                        continue
                    value = expected_ports[direction][name]
                    actual = self.source["values"][source]
                    expected = self.target.graph["values"][value]

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
        missing = self.target.operation_paths.missing(required)

        # A logical dependency also requires its producer's CPU return before the consuming call enters.
        calls = {
            (self.target.calls[source]["return"], self.target.calls[target]["entry"]): (source, target)
            for source, target in required
        }
        missing.update(calls[link] for link in self.target.execution_paths.missing(set(calls)))
        return sorted((positions[source], positions[target]) for source, target in missing)

