"""Check cross-rank links against native groups and registered copy endpoints."""

from __future__ import annotations

import json
import math
from collections import defaultdict
from itertools import zip_longest
from pathlib import Path
from typing import Any

from markov_collection.adapters.frameworks.sglang.registered_buffers import RegisteredBuffers
from operations import TargetOperations


class CommunicationComparison:
    def __init__(
        self, target: TargetOperations, ranks: list[dict[str, Any]], decisions: list[dict[str, Any]], page_size: int,
    ) -> None:
        self.target = target
        self.rank_count = len(ranks)
        self.page_size = page_size
        worlds = {
            directory: json.loads((directory / "session.json").read_text())["output"]
            for directory in {Path(case["source"]).parent for case in ranks}
        }
        self.worlds = {case["rank"]: worlds[Path(case["source"]).parent] for case in ranks}
        self.transfers = {
            (decision["task"]["group"], decision["task"]["sequence"]): decision
            for decision in decisions if decision["step"] == "transfer"
        }
        self.transports = {}
        for case in ranks:
            source = Path(case["source"])
            registration = source.with_name("transport-" + source.name)
            if registration.is_file():
                buffers = RegisteredBuffers(json.loads(registration.read_text()))
                if buffers.rank != case["rank"]:
                    raise ValueError("native registration rank differs from its observation")
                self.transports[buffers.session] = buffers

        self.joins = {
            frozenset(region["rank_operations"]): region
            for region in target.graph["regions"] if "rank_operations" in region
        }
        self.join_nodes = {
            node["region"]: node["id"] for node in target.graph["execution"]["nodes"]
            if node["kind"] == "communication" and "region" in node
        }
        self.copy_operations = {
            operation["id"] for operation in target.graph["operations"] if operation["name"] == "remote_copy"
        }
        self.checked_joins: set[int] = set()

    def observe(
        self, source: dict[str, Any], physical: dict[str, Any],
        operations: list[dict[str, Any]], paired: dict[int, dict[str, Any]],
    ) -> dict[str, Any]:
        rank = source["environment"]["rank"]
        expected = {operation["region"]: operation for operation in operations if operation.get("communication")}
        scopes = []
        for region in source["regions"]:
            parent = scopes[region["parent"]] if region["parent"] is not None else (None, None, False, None)
            metadata = region["metadata"]
            context = metadata if region["role"] in {"KvTransfer", "KvMetadataTransfer", "KvReceived"} else parent[3]
            scopes.append((
                expected.get(region["id"], parent[0]),
                metadata.get("room", parent[1]),
                region["role"] == "KvReceived" or parent[2],
                context,
            ))

        facts: dict[str, Any] = {
            "rank": rank, "world": self.worlds[rank], "groups": [], "calls": [], "copies": [], "received": [],
            "expected_calls": len(expected), "covered_calls": 0,
        }
        covered = set()
        if expected:
            for call in physical["communication"] or ():
                facts["groups"].append(call["group"])
                owner = call["owner"]
                if owner is None:
                    continue
                region = owner["region"]
                if region is None and owner["operation"] is not None:
                    region = source["operations"][owner["operation"]]["region"]
                if region is None:
                    continue
                operation = scopes[region][0]
                if operation is None:
                    continue

                covered.add(operation["id"])
                submission = call["submission"]

                # Rank publication carries only the correspondence needed for a resumed cross-rank comparison.
                facts["calls"].append({
                    "group": call["group"],
                    "primitive": call["primitive"].removeprefix("hcom_").strip("_").lower(),
                    "elements": call["elements"],
                    "order": submission["order"],
                    "target": paired[operation["id"]]["id"] if operation["id"] in paired else None,
                })
        facts["groups"] = sorted(set(facts["groups"]))
        facts["covered_calls"] = len(covered)

        sender = next((item for item in self.transports.values() if item.rank == rank), None)
        for operation in operations:
            metadata = source["regions"][operation["region"]]["metadata"]
            target = paired[operation["id"]]["id"] if operation["id"] in paired else None
            _, room, received, context = scopes[operation["region"]]
            if received:
                facts["received"].append({
                    "room": room, "target": target, "metadata_slot": context["parameters"]["metadata_slot"],
                })
            if operation["name"] != "remote_copy":
                continue

            destination = self.transports[metadata["destination_session"]]
            payload = sender.payload(destination, metadata, context)
            facts["copies"].append({
                "room": room, "target": target, "source": rank, "destination": destination.rank,
                "kind": payload["kind"], "payload": payload,
                "bytes": metadata["bytes"], "segments": len(metadata["segments"]),
                "completed": metadata["status"] == 0,
                **({"destination_metadata_slot": context["destination_metadata_slot"]}
                   if payload["kind"] == "metadata" else {}),
            })
        return facts

    def compare(self, observations: list[dict[str, Any]]) -> dict[str, Any]:
        groups: dict[tuple[str, str], set[int]] = defaultdict(set)
        calls: dict[tuple[tuple[str, str], str], dict[int, list[dict[str, Any]]]] = defaultdict(
            lambda: defaultdict(list)
        )
        received: dict[tuple[Any, int], list[dict[str, Any]]] = defaultdict(list)
        copies: dict[tuple[Any, int, int], dict[str, list[int]]] = defaultdict(lambda: defaultdict(list))
        metadata_slots: dict[tuple[Any, int], set[int]] = defaultdict(set)
        missing = self.rank_count - len(observations)
        checked = 0
        payload_slices = 0
        checked_copies = set()
        issues = []
        links: set[tuple[int, int]] = set()
        for observation in observations:
            rank = observation["rank"]
            missing += observation["expected_calls"] - observation["covered_calls"]

            # SDK group names belong to one native world; independent worlds may reuse the same name.
            for group in observation["groups"]:
                groups[observation["world"], group].add(rank)
            for call in observation["calls"]:
                calls[(observation["world"], call["group"]), call["primitive"]][rank].append(call)
            for entry in observation["received"]:
                received[entry["room"], rank].append(entry)
            for copy in observation["copies"]:
                if copy["kind"] == "metadata":
                    metadata_slots[copy["room"], copy["destination"]].add(copy["destination_metadata_slot"])
                target = copy["target"]
                if target is None or copy["kind"] is None:
                    missing += 1
                    continue
                region = self.target.graph["regions"][self.target.graph["operations"][target]["region"]]
                transfer = region["transfer"]
                parameters = region["parameters"]
                checked += 1
                checked_copies.add(target)
                if (
                    not copy["completed"]
                    or any(copy[field] != transfer[field] for field in ("source", "destination", "kind"))
                    or copy["bytes"] != parameters["transfer_bytes"]
                    or copy["segments"] != parameters["transfer_segments"]
                ):
                    issues.append({"kind": "copy_endpoint_or_extent", "operation": target})
                if copy["kind"] == "kv":
                    task = region["task"]
                    action = self.transfers[task["group"], task["sequence"]]
                    begin = action["members"][0]["context_tokens"]
                    end = begin + len(action["page_copies"]) * self.page_size
                    expected = sorted(
                        ({**slice, "tokens": [[begin, end]]} for slice in transfer["slices"]),
                        key=lambda item: tuple(item[name] for name in RegisteredBuffers.slice_fields),
                    )
                    actual = copy["payload"]["slices"]
                    payload_slices += len(actual)
                    if actual != expected:
                        issues.append({
                            "kind": "copy_layer_head_or_tokens", "operation": target,
                            "native": actual[:2], "target": expected[:2],
                        })
                copies[copy["room"], copy["source"], copy["destination"]][copy["kind"]].append(target)

        for (group, primitive), participants in calls.items():
            for sequence in participants.values():
                sequence.sort(key=lambda call: call["order"])
            if primitive in {"send", "receive"}:
                continue
            for row in zip_longest(*(participants[rank] for rank in sorted(groups[group]))):
                count, absent = self.check_join(row, links, issues)
                checked += count
                missing += absent

        point_groups = {group for group, primitive in calls if primitive in {"send", "receive"}}
        for group in point_groups:

            # A unique native peer is required; the logical PP group name supplies no evidence.
            if len(groups[group]) != 2:
                missing += sum(
                    len(sequence) for primitive in ("send", "receive") for sequence in calls[group, primitive].values()
                )
                continue
            for source in groups[group]:
                (destination,) = groups[group] - {source}
                for row in zip_longest(calls[group, "send"][source], calls[group, "receive"][destination]):
                    count, absent = self.check_join(row, links, issues, source, destination)
                    checked += count
                    missing += absent

        for (room, _, destination), fragments in copies.items():
            consumers = received[room, destination]
            if not fragments["metadata"] or not consumers:
                missing += 1
            for acknowledgement in fragments["metadata"]:
                ports = self.target.calls[acknowledgement]
                links.update((self.target.calls[operation]["return"], ports["entry"]) for operation in fragments["kv"])
                for consumer in consumers:
                    operation = consumer["target"]
                    if metadata_slots[room, destination] != {consumer["metadata_slot"]}:
                        issues.append({"kind": "native_metadata_consumer_row", "operation": operation})
                    if operation is None:
                        missing += 1
                    else:
                        links.add((ports["return"], self.target.calls[operation]["entry"]))

        missing_links = self.target.execution_paths.missing(links)
        if missing_links:
            issues.append({
                "kind": "completion_links", "count": len(missing_links), "examples": sorted(missing_links)[:3],
            })
        missing += len(self.joins) - len(self.checked_joins) + len(self.copy_operations - checked_copies)
        return {
            "scope": (
                "native SDK groups, matched transport endpoints and completion ports; "
                "registered PD layer/head/logical-token intervals and metadata rows, not numerical KV values"
            ),
            "status": "differences" if issues else "partial" if missing else "matched",
            "checked": checked, "unverified": missing, "completion_links": len(links),
            "payload_slices": payload_slices,
            "issues": issues[:10], "issue_count": len(issues),
        }

    def check_join(
        self, calls: tuple[dict[str, Any] | None, ...],
        links: set[tuple[int, int]], issues: list[dict[str, Any]],
        source: int | None = None, destination: int | None = None,
    ) -> tuple[int, int]:
        if any(call is None or call["target"] is None for call in calls):
            return 0, 1
        operations = [call["target"] for call in calls]
        region = self.joins.get(frozenset(operations))
        if region is None:
            issues.append({"kind": "native_peer_pairing", "operations": operations})
            return 1, 0

        specification = region["communication"]
        self.checked_joins.add(region["id"])
        if (source is None and region["role"] != "collective") or (
            source is not None and (
                specification["primitive"] != "send_receive"
                or (specification["source"], specification["target"]) != (source, destination)
            )
        ):
            issues.append({"kind": "native_group_or_direction", "region": region["id"]})

        joined = self.join_nodes[region["id"]]
        for call, operation in zip(calls, operations, strict=True):
            ports = self.target.communication_ports[operation]
            links.update(((ports["entry"], joined), (joined, ports["return"])))
            if source is not None:
                access = self.target.graph["operations"][operation]
                values = access["outputs" if call["primitive"] == "receive" else "inputs"]
                elements = math.prod(self.target.graph["values"][values[0]]["shape"])
                if elements != call["elements"]:
                    issues.append({"kind": "native_payload_elements", "operation": operation})
        return 1, 0
