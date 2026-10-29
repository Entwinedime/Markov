#!/usr/bin/env python3
"""Compare warmed native device work with and without structural observation."""

from __future__ import annotations

import argparse
import json
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any

from markov_collection.adapters.runtimes.ascend.trace import AscendTrace
from markov_collection.artifacts import write_json


class PhysicalComparison:
    def __init__(self, source: Path, trace: Path, runtime: Path | None = None) -> None:
        self.source = json.loads(source.read_text())
        self.trace = AscendTrace(trace, runtime, device=self.source["environment"]["device_index"])

    @staticmethod
    def signature(batch: dict[str, Any]) -> tuple[Any, ...]:
        return (
            batch["phase"],
            tuple(sorted(batch["parameters"].items())),
            tuple((member["new_tokens"], member["context_tokens"]) for member in batch["members"]),
        )

    def batch(self, region: int) -> int:
        while True:
            record = self.source["regions"][region]
            if "batch" in record["metadata"]:
                return record["metadata"]["batch"]
            if record["parent"] is None:
                raise ValueError(f"observed region {region} has no batch")
            region = record["parent"]

    @staticmethod
    def event_protocol(actions: list[dict[str, Any]]) -> list[tuple[str, int, int | None]] | None:
        events: dict[int, int] = {}
        streams: dict[int, int] = {}
        result = []
        ordered = sorted(
            actions,
            key=lambda action: (
                action["submission"]["queue"]["enqueue_order"]
                if action["submission"]["queue"] is not None
                else action["submission"]["order"]
            ),
        )
        for action in ordered:
            call = action["native_call"]
            host_wait = action["kind"] == "wait_event_complete"
            if call is None or "event" not in call or (not host_wait and call["stream_status"] != 0):
                return None
            result.append(
                (
                    action["kind"],
                    events.setdefault(call["event"], len(events)),
                    None if host_wait else streams.setdefault(call["stream"], len(streams)),
                )
            )
        return result

    def compare(self, physical: dict[str, Any]) -> dict[str, Any]:
        work: dict[int, Counter[str]] = defaultdict(Counter)
        non_event_work: dict[int, Counter[str]] = defaultdict(Counter)
        runtime: dict[int, Counter[str]] = defaultdict(Counter)
        event_actions: dict[int, list[dict[str, Any]]] = defaultdict(list)
        assigned = Counter()
        outside = Counter()
        for task in physical["work"]:
            if task["observer"] or task["auxiliary"]:
                continue

            # Direct Triton launches can belong to a native region without a torch dispatch operation.
            if task["baseline"] is not None:
                batch = task["baseline"]
            elif task["region"] is not None:
                batch = self.batch(task["region"])
                owner = (
                    self.source["operations"][task["operation"]]["name"]
                    if task["operation"] is not None
                    else "native_region"
                )
                assigned[owner] += 1
            else:
                outside[task["name"]] += 1
                continue
            work[batch][task["name"]] += 1
            if task["task_type"] not in ("EVENT_RECORD", "EVENT_WAIT"):
                non_event_work[batch][task["name"]] += 1

        for action in physical["runtime_actions"]:
            if action["observer"] or action["auxiliary"]:
                continue
            if action["baseline"] is not None:
                batch = action["baseline"]
            elif action["region"] is not None:
                batch = self.batch(action["region"])
            else:
                continue
            runtime[batch][action["native_api"]] += 1
            if action["kind"] in ("record_event", "wait_event", "wait_event_complete"):
                event_actions[batch].append(action)

        groups: dict[tuple[Any, ...], dict[str, list[int]]] = {}
        for batch in self.source["batches"]:
            group = groups.setdefault(self.signature(batch), {"baseline": [], "source": []})
            group[batch["observation"]].append(batch["id"])

        comparisons = []
        markers = {event["name"] for event in self.trace.events if event.get("ph") == "X"}
        source_batches = {
            region["metadata"]["batch"]
            for region in self.source["regions"]
            if "batch" in region["metadata"] and f"markov.region/{region['id']}" in markers
        }
        for group in groups.values():
            paired = len(group["baseline"]) == len(group["source"]) and bool(group["baseline"])
            protocols = {
                identity: self.event_protocol(event_actions[identity])
                for identities in group.values()
                for identity in identities
            }
            for side, identities in group.items():
                for identity in identities:
                    captured = (
                        identity in source_batches if side == "source" else f"markov.baseline/{identity}" in markers
                    )
                    if not captured:
                        raise ValueError(f"no complete native marker for {side} batch {identity}")
            comparisons.append(
                {
                    "batch": self.source["batches"][(group["source"] or group["baseline"])[0]],
                    "baseline_batches": group["baseline"],
                    "source_batches": group["source"],
                    "baseline_tasks": [dict(work[identity]) for identity in group["baseline"]],
                    "source_tasks": [dict(work[identity]) for identity in group["source"]],
                    "baseline_runtime": [dict(runtime[identity]) for identity in group["baseline"]],
                    "source_runtime": [dict(runtime[identity]) for identity in group["source"]],
                    "same_runtime_effects": all(
                        runtime[left] == runtime[right]
                        for left, right in zip(group["baseline"], group["source"], strict=True)
                    )
                    if paired
                    else None,
                    "same_device_work": all(
                        work[left] == work[right]
                        for left, right in zip(group["baseline"], group["source"], strict=True)
                    )
                    if paired
                    else None,
                    "same_non_event_work": all(
                        non_event_work[left] == non_event_work[right]
                        for left, right in zip(group["baseline"], group["source"], strict=True)
                    )
                    if paired
                    else None,
                    "same_event_protocol": all(
                        protocols[left] == protocols[right]
                        for left, right in zip(group["baseline"], group["source"], strict=True)
                    )
                    if paired and all(protocol is not None for protocol in protocols.values())
                    else None,
                }
            )

        report = {
            "comparisons": comparisons,
            "assigned_by_operation": dict(assigned),
            "outside_selected_batches": dict(outside),
            "unassociated_tasks": physical["unassociated_tasks"],
            "unassociated_submissions": [
                task["source_event"] for task in physical["work"] if task["submission"] is None
            ],
            "submission_entries": dict(
                Counter(
                    task["submission"]["native_entry"] for task in physical["work"] if task["submission"] is not None
                )
            ),
            "unassociated_runtime_actions": physical["unassociated_runtime_actions"],
            "observer_tasks": sum(task["observer"] for task in physical["work"]),
            "auxiliary_tasks": sum(task["auxiliary"] for task in physical["work"]),
            "observer_controls": physical["observer_controls"],
            "scope": "device/API inventories and event protocol; target structure and timing remain unqualified",
        }
        states = self.source["environment"].get("workspace_boundaries")
        report["serving_workspace"] = (
            {
                "boundaries": states,
                "same_initial_state": states["warmup"] == states["baseline"] if "baseline" in states else None,
                "unchanged_capacity": all(state == states["warmup"] for state in states.values()),
                "scope": "retained stream allocator state at serving window boundaries; primitive groups excluded",
            }
            if states is not None
            else None
        )
        return report

    def write(self, output: Path) -> dict[str, Any]:
        physical = self.trace.physical_work(len(self.source["regions"]), len(self.source["operations"]))
        report = self.compare(physical)
        write_json(output / "physical.json", physical)
        write_json(output / "physical-summary.json", report, indent=2)
        return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--trace", type=Path, required=True)
    parser.add_argument("--runtime", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    report = PhysicalComparison(arguments.source, arguments.trace, arguments.runtime).write(arguments.output)
    print(
        json.dumps(
            {
                "compared_groups": sum(item["same_device_work"] is not None for item in report["comparisons"]),
                "unassociated": len(report["unassociated_tasks"]),
            }
        )
    )
