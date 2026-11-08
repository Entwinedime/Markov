"""Choose a finite SGLang source workload from declared input opportunities."""

import json

from markov_adapters.sglang.configuration import ServingInput
from markov_workload import WorkloadContent
from markov_workflow.adapters.sglang_probes import LifecycleProbes


class RepresentativeSources:
    """Combine compatible input witnesses without claiming native branch coverage."""

    def __init__(self, workloads: dict[str, dict]) -> None:
        self.workloads = workloads
        self.contents = {name: WorkloadContent(value["requests"], value["prefixes"]) for name, value in workloads.items()}

    def conditions(self, serving: ServingInput, workload: str) -> set[str]:
        requests = self.workloads[workload]["requests"]
        conditions = {"prefill"}
        if any(request["output_tokens"] == 1 for request in requests):
            conditions.add("prefill_completion")
        if any(request["output_tokens"] > 1 for request in requests):
            conditions.add("decode")
        if any(request["output_tokens"] > 2 for request in requests):
            conditions.add("repeated_decode")
        if len({request["arrival_ns"] for request in requests}) > 1:
            conditions.add("later_arrival")
        for instance in serving.instances:
            config = instance.configuration.values
            prefix = instance.mode + "/"
            if config["max_running_requests"] > 1 and len(requests) > 1:
                conditions.add(prefix + "multiple_requests")
                if len({request["output_tokens"] for request in requests}) > 1:
                    conditions.add(prefix + "unequal_output_lengths")
            chunk = config["prefill_chunk_tokens"]
            if chunk and any(request["prompt_tokens"] > chunk for request in requests):
                conditions.add(prefix + "chunk_continuation")
            if any(request["prompt_tokens"] % config["page_size"] for request in requests):
                conditions.add(prefix + "partial_page")
            if config["prefix_cache"] and LifecycleProbes.shared_prefix_requests(self.contents[workload], config["page_size"]):
                conditions.add(prefix + "shared_prefix")
        return conditions

    @staticmethod
    def cost(workload, instances) -> dict:
        requests = workload["requests"]
        return {
            "requests": len(requests),
            "prompt_tokens": sum(request["prompt_tokens"] for request in requests),
            "decode_tokens": sum(request["output_tokens"] - 1 for request in requests),
            "last_arrival_ns": max(request["arrival_ns"] for request in requests),
            "worker_kv_tokens": sum(instance.configuration.tp * instance.configuration.pp
                                    * instance.configuration.values["max_total_tokens"] for instance in instances),
        }

    @staticmethod
    def padding(serving: ServingInput) -> tuple:
        result = {}
        for instance in serving.instances:
            config = instance.configuration
            routes = config.routes(serving.workload)
            if routes is not None:
                entries = routes.get("padding", [])
                result[config.values["ep"]["id"]] = tuple(sorted(json.dumps(entry, sort_keys=True) for entry in entries))
        return tuple(sorted(result.items()))

    def combine(self, names: list[str], topologies: list[str], workloads: dict) -> tuple[dict, list[list[str]]]:
        workload = {"id": "representative", "requests": [], "prefixes": []}
        routes, groups = {}, []
        for name in names:
            source = workloads[name]

            # Namespace complete identities, including separators inside user IDs; preserve each prefix tree.
            identities = {identity: json.dumps([name, identity], ensure_ascii=False, separators=(",", ":"))
                          for identity in {entry["id"] for entry in source["requests"] + source["prefixes"]}}
            groups.append([identities[request["id"]] for request in source["requests"]])
            for field, parent in (("requests", "prefix_id"), ("prefixes", "parent")):
                for entry in source[field]:
                    copied = {**entry, "id": identities[entry["id"]]}
                    if entry.get(parent) is not None:
                        copied[parent] = identities[entry[parent]]
                    workload[field].append(copied)
            for topology in topologies:
                route = source["ep_routes"][topology]
                combined = routes.setdefault(topology, {**route, "assignments": []})
                combined["assignments"].extend({**entry, "request": identities[entry["request"]]}
                                               for entry in route["assignments"])
        if routes:
            workload["ep_routes"] = routes
        return workload, groups

    def select(self, candidates: list[tuple[str, str, ServingInput]]) -> tuple[dict, dict, list[list[str]]]:
        groups, instances = {}, {}
        for configuration, name, serving in candidates:
            instances[configuration] = serving.instances
            groups.setdefault((configuration, self.padding(serving)), []).append({
                "name": name, "conditions": self.conditions(serving, name),
                "cost": self.cost(serving.workload, serving.instances),
            })
        required = set().union(*(item["conditions"] for group in groups.values() for item in group))
        choices = []
        for (configuration, padding), remaining in groups.items():
            selected, covered = [], set()
            while remaining:
                useful = [item for item in remaining if item["conditions"] - covered]
                if not useful:
                    break

                # Each addition supplies a new declared condition; source count cannot grow through retries.
                item = min(useful, key=lambda item: (
                    -len(item["conditions"] - covered),
                    item["cost"]["prompt_tokens"] + item["cost"]["decode_tokens"],
                    item["cost"]["last_arrival_ns"], item["cost"]["requests"], item["name"],
                ))
                selected.append(item)
                remaining.remove(item)
                covered.update(item["conditions"])

            # A later witness can subsume an earlier workload; discard whole redundant inputs, never individual requests.
            for item in sorted(selected, key=lambda item: (
                item["cost"]["prompt_tokens"] + item["cost"]["decode_tokens"], item["name"],
            ), reverse=True):
                others = [other for other in selected if other is not item]
                if covered <= set().union(*(other["conditions"] for other in others)):
                    selected.remove(item)
            names = sorted(item["name"] for item in selected)
            topologies = [name for name, _ in padding]
            probes = LifecycleProbes(instances[configuration], topologies)
            normalized = {name: probes.normalize(self.workloads[name]) for name in names}
            workload, submissions = self.combine(names, topologies, normalized)
            probes.extend(workload, submissions)
            cost = self.cost(workload, instances[configuration])
            cost.pop("last_arrival_ns")
            requests = {request["id"]: request for request in workload["requests"]}
            cost["arrival_wait_ns"] = sum(max(requests[name]["arrival_ns"] for name in group) for group in submissions)
            choices.append({"configuration": configuration, "workloads": names,
                            "input_cost": cost, "input_conditions": sorted(covered),
                            "original_tokens": sum(item["cost"]["prompt_tokens"] + item["cost"]["decode_tokens"] for item in selected),
                            "submission_group_count": len(submissions),
                            "unrepresented_conditions": sorted(required - covered),
                            "workload": workload, "submission_groups": submissions})
        choice = min(choices, key=lambda item: (
            len(item["unrepresented_conditions"]), item["input_cost"]["prompt_tokens"] + item["input_cost"]["decode_tokens"],
            item["input_cost"]["arrival_wait_ns"], item["input_cost"]["worker_kv_tokens"],
            item["input_cost"]["requests"], item["configuration"], item["workloads"],
        ))
        workload, submissions = choice.pop("workload"), choice.pop("submission_groups")
        return choice, workload, submissions
