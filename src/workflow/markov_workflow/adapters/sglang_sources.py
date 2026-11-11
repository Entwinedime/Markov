"""Choose a finite SGLang source workload from declared input opportunities."""

import json

from markov_adapters.sglang.allocation import AllocationABI
from markov_adapters.sglang.configuration import NativeConfiguration, ServingInput
from markov_workload import WorkloadContent
from markov_workflow.adapters.sglang_probes import LifecycleProbes


class RepresentativeSources:
    """Combine compatible input witnesses without claiming native branch coverage."""

    def __init__(self, workloads: dict[str, dict]) -> None:
        self.workloads = workloads

    @staticmethod
    def prefill_page_bounds(config, requests, resident_pages):
        page_size = config["page_size"]
        capacity = config["max_total_tokens"] // page_size
        members = NativeConfiguration.batch_capacity(config)
        cohorts = {}
        for request, total in zip(requests, resident_pages, strict=True):
            if total <= capacity:
                pages = (request["prompt_tokens"] + page_size - 1) // page_size
                cohorts.setdefault(request["arrival_ns"], []).append(pages)

        bound, repeated = 0, 0
        for pages in cohorts.values():
            pages.sort()
            chunk = config["prefill_chunk_tokens"]
            input_budget = (config["max_prefill_tokens"] + page_size - 1) // page_size

            # Native ignore-EOS admission checks the input budget after adding a whole request.
            budget = ((chunk + page_size - 1) // page_size if chunk else
                      input_budget - 1 + pages[-1])
            cohort_bound = min(capacity, budget, sum(pages[-members:]))
            bound = max(bound, cohort_bound)

            # A later write needs a second member in this call, not merely another request in the workload.
            if (members > 1 and len(pages) > 1 and sum(pages[:2]) <= capacity
                    and pages[0] < min(input_budget, budget)):
                repeated = max(repeated, cohort_bound)
        return bound, repeated

    @staticmethod
    def conditions(instances, workload: dict) -> set[str]:
        requests = workload["requests"]
        content = WorkloadContent(requests, workload["prefixes"])
        decode_cohorts = {}
        for request in requests:
            if request["output_tokens"] > 1:
                decode_cohorts.setdefault(request["arrival_ns"], []).append(request["output_tokens"])
        conditions = {"prefill"}
        if any(request["output_tokens"] == 1 for request in requests):
            conditions.add("prefill_completion")
        if any(request["output_tokens"] > 1 for request in requests):
            conditions.add("decode")
        if any(request["output_tokens"] > 2 for request in requests):
            conditions.add("repeated_decode")
        if len({request["arrival_ns"] for request in requests}) > 1:
            conditions.add("later_arrival")
        for instance in instances:
            config = instance.configuration.values
            prefix = instance.mode + "/"
            if NativeConfiguration.batch_capacity(config) > 1 and len(requests) > 1:
                conditions.add(prefix + "multiple_requests")
                if len({request["output_tokens"] for request in requests}) > 1:
                    conditions.add(prefix + "unequal_output_lengths")
                if any(len(lengths) > 1 for lengths in decode_cohorts.values()):
                    conditions.add(prefix + "concurrent_decode_inputs")
                if any(len(set(lengths)) > 1 for lengths in decode_cohorts.values()):
                    conditions.add(prefix + "decode_turnover_inputs")

            # Prefill hands off after its prompt; other roles retain KV as Decode extends each request.
            page_size = config["page_size"]
            pages = [(request["prompt_tokens"] + (0 if instance.mode == "prefill" else request["output_tokens"] - 1)
                      + page_size - 1) // page_size for request in requests]

            # PD defers returned pages even without caching; sharing and admission still decide actual pressure.
            if (config["prefix_cache"] or instance.mode != "null") and max(pages) <= config["max_total_tokens"] // page_size < sum(pages):
                conditions.add(prefix + "aggregate_kv_demand_exceeds_pool")
            if instance.mode != "decode":

                # A cold-input bound preserves the allocator path opportunity; native admission may still split it.
                bound, repeated = RepresentativeSources.prefill_page_bounds(config, requests, pages)
                if repeated:
                    conditions.add(prefix + "multiple_prefill_inputs")
                if page_size > 1 and AllocationABI.extend_branch(bound) == "extend_naive":
                    conditions.add(prefix + "naive_allocation_inputs")
                if page_size > 1 and AllocationABI.extend_branch(repeated) == "extend_naive":
                    conditions.add(prefix + "naive_repeated_allocation_inputs")
            chunk = config["prefill_chunk_tokens"]
            if chunk and any(request["prompt_tokens"] > chunk for request in requests):
                conditions.add(prefix + "chunk_continuation")
            if any(request["prompt_tokens"] % config["page_size"] for request in requests):
                conditions.add(prefix + "partial_page")
            if config["prefix_cache"] and LifecycleProbes.shared_prefix_requests(content, page_size):
                conditions.add(prefix + "shared_prefix")
        return conditions

    @staticmethod
    def model_coverage(mode, configuration, conditions):
        if configuration["tp"] == 1 or NativeConfiguration.batch_capacity(configuration) == 1:
            return set()

        # PD consumes the transferred first token before choosing a Decode model batch.
        if mode == "decode" and "decode" not in conditions:
            return set()

        # Ordinary batches need a declared input opportunity; PD readiness can combine separately admitted requests.
        if mode == "null" and not {"null/multiple_prefill_inputs", "null/concurrent_decode_inputs"}.intersection(conditions):
            return set()
        return {"single_model_batch", "multiple_model_batch"}

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
                "name": name, "conditions": self.conditions(serving.instances, self.workloads[name]),
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
            normalized = {}
            for item in selected:
                name, conditions = item["name"], item["conditions"]
                original = self.workloads[name]
                normalized[name] = original

                # Fixed arrival gaps cannot be rescaled without knowing native execution time.
                if "later_arrival" not in conditions:
                    shortened = probes.normalize(original)
                    shortened_conditions = self.conditions(instances[configuration], shortened)
                    if conditions <= shortened_conditions:
                        normalized[name] = shortened
                        covered.update(shortened_conditions)
            workload, submissions = self.combine(names, topologies, normalized)
            single_model = any(self.model_coverage(instance.mode, instance.configuration.values, covered)
                               for instance in instances[configuration])
            probes.extend(workload, submissions, single_model=single_model)
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
