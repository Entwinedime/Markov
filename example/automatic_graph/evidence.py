"""Check emitted execution and native routing facts without changing either side."""

from __future__ import annotations

import argparse
import json
from collections import Counter, defaultdict
from itertools import pairwise
from pathlib import Path
from typing import Any

from markov_collection.artifacts.json_file import write_json


class ExecutionEvidence:
    def __init__(self, result: dict[str, Any]) -> None:
        self.result = result

    def check(self) -> dict[str, Any]:
        graph = self.result["graph"]
        execution = graph["execution"]
        timeline = {item["node"]: item for item in self.result["execution_timeline"]}
        identities = {node["id"] for node in execution["nodes"]}
        checks = {
            "all_nodes_completed_once": len(timeline) == len(self.result["execution_timeline"])
            and set(timeline) == identities
            and all(item["status"] == "completed" for item in timeline.values()),
            "nonnegative_intervals": all(
                0 <= item["started_ns"] <= item["completed_ns"] <= self.result["completed_ns"]
                for item in timeline.values()
            ),
        }
        if not checks["all_nodes_completed_once"]:
            return {"status": "differences", "checks": checks}
        checks["dependency_readiness"] = all(
            timeline[edge["source"]]["completed_ns"] <= timeline[edge["target"]]["started_ns"]
            for edge in execution["dependencies"]
        )
        resources = defaultdict(list)
        actions = defaultdict(list)
        owner = {}
        for node in execution["nodes"]:
            item = timeline[node["id"]]
            resources[node["resource"]].append((item["started_ns"], item["completed_ns"]))
            region = node.get("region")
            if region is None:
                region = graph["operations"][node["operation"]]["region"]
            action = graph["regions"][region]["action"]
            actions[action].append(item)
            owner[node["id"]] = action
        checks["exclusive_resources"] = all(
            left[1] <= right[0] for intervals in resources.values() for left, right in pairwise(sorted(intervals))
        )
        decisions = self.result["decisions"]
        checks["all_actions_drained"] = set(actions) == {item["task"]["sequence"] for item in decisions} and all(
            item["scheduled_ns"] <= node["started_ns"] <= node["completed_ns"] <= item["completed_ns"]
            for item in decisions
            for node in actions[item["task"]["sequence"]]
        )
        outputs = defaultdict(list)
        for item in self.result["outputs"]:
            outputs[item["request"]].append(item)
        checks["published_outputs"] = set(outputs) == {item["id"] for item in self.result["requests"]} and all(
            [item["token"] for item in outputs[request["id"]]] == list(range(1, request["generated_tokens"] + 1))
            and outputs[request["id"]][0]["time_ns"] == request["first_output_ns"]
            and outputs[request["id"]][-1]["time_ns"] == request["last_output_ns"]
            and all(left["time_ns"] <= right["time_ns"] for left, right in pairwise(outputs[request["id"]]))
            for request in self.result["requests"]
        )

        # Cross-instance ownership needs explicit edges as well as a legal execution time.
        flows = defaultdict(dict)
        for item in decisions:
            if item["step"] in {"reserve", "model", "transfer", "accept", "release"}:
                flows[item["flow"]][item["step"]] = item
        cross = {(owner[edge["source"]], owner[edge["target"]]) for edge in execution["dependencies"]}
        checks["handoff_steps"] = all(
            set(steps) == {"reserve", "model", "transfer", "accept", "release"} for steps in flows.values()
        )
        checks["handoff_readiness"] = all(
            steps[first]["completed_ns"] <= steps[last]["scheduled_ns"]
            and (steps[first]["task"]["sequence"], steps[last]["task"]["sequence"]) in cross
            for steps in flows.values()
            if set(steps) == {"reserve", "model", "transfer", "accept", "release"}
            for first, last in (
                ("reserve", "transfer"),
                ("model", "transfer"),
                ("transfer", "accept"),
                ("transfer", "release"),
            )
        )
        return {
            "status": "matched" if all(checks.values()) else "differences",
            "checks": checks,
            "execution_nodes": len(timeline),
            "actions": len(decisions),
            "handoffs": len(flows),
        }


class NativeEvidence:
    def __init__(self, sources: list[Path], target: dict[str, Any], minimum_members: int) -> None:
        self.sources = sources
        self.target = target
        self.minimum_members = minimum_members
        self.groups = defaultdict(dict)
        self.maximum_members = {}
        self.mismatches = []
        self.difference_count = 0
        self.unobserved = 0

    def difference(self, key: tuple, check: str) -> None:
        self.difference_count += 1
        if len(self.mismatches) < 8:
            self.mismatches.append({"call": key, "check": check})

    def collect(self, path: Path) -> None:
        source = json.loads(path.read_text())
        session = json.loads((path.parent / "session.json").read_text())
        names = dict(zip(session["source_requests"], (item["id"] for item in self.target["requests"]), strict=True))
        environment = source["environment"]
        instance = environment["implementation"]["disaggregation_mode"]
        instance = "model" if instance == "null" else instance
        batches = {batch["id"]: batch for batch in source["batches"] if batch["observation"] == "source"}
        self.maximum_members[environment["rank"]] = 0
        labels = []
        occurrences = Counter()
        for region in source["regions"]:
            metadata = region["metadata"]
            batch = metadata.get("batch")
            if batch is None and region["parent"] is not None:
                batch = labels[region["parent"]]
            labels.append(batch)
            if region["role"] in {"Prefill", "Decode"}:
                self.maximum_members[environment["rank"]] = max(
                    self.maximum_members[environment["rank"]], len(batches[batch]["members"])
                )
            if metadata.get("entrypoint") != "expert_layer":
                continue
            members = batches[batch]["members"]
            signature = tuple((names[item["id"]], item["new_tokens"], item["context_tokens"]) for item in members)
            key = (
                instance,
                environment["pp_rank"],
                batches[batch]["phase"],
                signature,
                metadata["bindings"]["expert_layer"],
            )
            occurrences[key] += 1
            key = (*key, occurrences[key])
            self.groups[key][metadata["bindings"]["expert_rank"]] = {
                field: metadata[field] for field in ("bindings", "parameters", "source_routing", "attributes")
            }

    @staticmethod
    def choices(assignments: list[dict[str, Any]], layer: int, token: int, request: str | None = None) -> list[int]:
        matches = [
            item["experts"]
            for item in assignments
            if item["layers"][0] <= layer < item["layers"][1]
            and item["tokens"][0] <= token < item["tokens"][1]
            and (request is None or item["request"] == request)
        ]
        if len(matches) != 1:
            raise ValueError("native logical token does not have exactly one literal routing assignment")
        return matches[0]

    def check(self) -> dict[str, Any]:
        for path in self.sources:
            self.collect(path)
        rows = padding = calls = 0
        for key, peers in self.groups.items():
            instance, _, _, members, layer, _ = key
            size = next(iter(peers.values()))["attributes"]["ep_size"]
            if set(peers) != set(range(size)):
                self.unobserved += 1
                continue
            settings = self.target["configuration"]
            pool = settings["pd"][instance] if "pd" in settings else settings
            route = self.target["ep_routes"][pool["ep"]["id"]]
            actual = [row for peer in range(size) for row in peers[peer]["source_routing"]]

            # Literal interval lookup is independent of the injection and target shape/routing helpers.
            expected = [
                self.choices(route["assignments"], layer, token, request)
                for request, count, context in members
                for token in range(context, context + count)
            ]
            real = len(expected)
            expected += [self.choices(route["padding"], layer, token) for token in range(len(actual) - real)]
            if actual != expected:
                self.difference(key, "consumed_expert_ids")
            histograms = {
                peer: Counter(expert for row in value["source_routing"] for expert in row)
                for peer, value in peers.items()
            }
            total = sum(histograms.values(), Counter())
            for peer, metadata in peers.items():
                width = metadata["bindings"]["local_expert_count"]
                parameters = metadata["parameters"]
                expected_counts = {
                    "expert_received": sum(total[expert] for expert in range(peer * width, (peer + 1) * width))
                }
                if size > 1:
                    expected_counts.update(
                        {f"expert_{expert}": total[peer * width + expert] for expert in range(width)}
                    )
                    for other in range(size):
                        expected_counts[f"expert_send_{other}"] = sum(
                            histograms[peer][expert] for expert in range(other * width, (other + 1) * width)
                        )
                        expected_counts[f"expert_receive_{other}"] = sum(
                            histograms[other][expert] for expert in range(peer * width, (peer + 1) * width)
                        )
                if not expected_counts.keys() <= parameters.keys():
                    self.unobserved += 1
                elif any(parameters[name] != value for name, value in expected_counts.items()):
                    self.difference(key, f"native_split_and_expert_counts/peer-{peer}")
            calls += len(peers)
            rows += len(actual)
            padding += len(actual) - real
        if "ep_routes" in self.target and not self.groups:
            self.unobserved += 1
        batch_covered = max(self.maximum_members.values(), default=0) >= self.minimum_members
        status = (
            "differences" if self.difference_count or not batch_covered else "partial" if self.unobserved else "matched"
        )
        return {
            "status": status,
            "minimum_batch_members": self.minimum_members,
            "maximum_batch_members_by_rank": self.maximum_members,
            "batch_size_covered": batch_covered,
            "routing_calls": calls,
            "routing_rows": rows,
            "padding_rows": padding,
            "unobserved_checks": self.unobserved,
            "difference_count": self.difference_count,
            "difference_examples": self.mismatches,
        }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    subject = parser.add_mutually_exclusive_group(required=True)
    subject.add_argument("--prediction", type=Path)
    subject.add_argument("--source", type=Path, nargs="+")
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--minimum-batch-members", type=int, default=1)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    if arguments.prediction:
        report = ExecutionEvidence(json.loads(arguments.prediction.read_text())).check()
    else:
        report = NativeEvidence(
            arguments.source, json.loads(arguments.input.read_text()), arguments.minimum_batch_members
        ).check()
    write_json(arguments.output, report, indent=2)
    print(json.dumps({"status": report["status"], "report": str(arguments.output)}))
