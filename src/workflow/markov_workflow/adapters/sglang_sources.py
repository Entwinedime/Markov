"""Choose a finite SGLang source workload from declared input opportunities."""

import json

from markov_adapters.sglang.allocation import AllocationABI
from markov_adapters.sglang.configuration import NativeConfiguration, ServingInput
from markov_adapters.sglang.resources import CacheInputs
from markov_workload import WorkloadContent
from markov_workflow.adapters.sglang_probes import LifecycleProbes


class RepresentativeSources:
    """Combine compatible input witnesses without claiming native branch coverage."""

    def __init__(self, workloads: dict[str, dict], expert_count: int | None) -> None:
        self.workloads = workloads
        self.expert_count = expert_count

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

    def conditions(self, instances, workload: dict) -> set[str]:
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
            page_size = config["page_size"]
            prefix = instance.mode + "/"
            conditions.update(prefix + condition for condition in self.route_conditions(instance, workload))
            if config["prefix_cache"]:
                conditions.update(prefix + condition for condition in self.cache_conditions(instance.mode, config, content))
                if self.cached_page_demand(instance.mode, page_size, content) > config["max_total_tokens"] // page_size:
                    conditions.add(prefix + "cached_pages_exceed_pool")
            if NativeConfiguration.batch_capacity(config) > 1 and len(requests) > 1:
                conditions.add(prefix + "multiple_requests")
                if len({request["output_tokens"] for request in requests}) > 1:
                    conditions.add(prefix + "unequal_output_lengths")
                if any(len(lengths) > 1 for lengths in decode_cohorts.values()):
                    conditions.add(prefix + "concurrent_decode_inputs")
                if any(len(set(lengths)) > 1 for lengths in decode_cohorts.values()):
                    conditions.add(prefix + "decode_turnover_inputs")

            # Prefill hands off after its prompt; other roles retain KV as Decode extends each request.
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
    def cached_page_demand(mode, page_size, content):
        pages, shared = 0, set()
        for request in content.requests.values():
            tokens = request["prompt_tokens"] + (0 if mode == "prefill" else request["output_tokens"] - 1)
            prefix = request.get("prefix_id")
            pages += tokens // page_size - content.lengths[prefix] // page_size

            # Shared token paths count once; conditional expert routes can split them further.
            while prefix is not None and prefix not in shared:
                shared.add(prefix)
                parent = content.prefixes[prefix].get("parent")
                pages += content.lengths[prefix] // page_size - content.lengths[parent] // page_size
                prefix = parent
        return pages

    @staticmethod
    def lengths(workload):
        requests = workload["requests"]
        return {
            "prompt": {request["prompt_tokens"] for request in requests},
            "decode_context": {request["prompt_tokens"] + request["output_tokens"] - 2
                               for request in requests if request["output_tokens"] > 1},
        }

    @classmethod
    def length_evidence(cls, workload, bounds):
        if not bounds:
            return set()
        observed = cls.lengths(workload)
        return {f"length/{axis}/{point}" for axis, points in bounds.items() for point in observed[axis] & points}

    def reduce_requests(self, instances, workload, required, bounds):
        reduced = workload
        minimum = 1
        for instance in instances:
            config = instance.configuration.values
            resident = min(config["max_running_requests"], config["pp"] * NativeConfiguration.batch_capacity(config))
            if bounds:
                minimum = max(minimum, min(len(workload["requests"]), resident))
            if instance.mode == "null" and config["mixed_batches"] and "decode" in required:
                minimum = max(minimum, min(len(workload["requests"]), resident + 1))

        ordered = sorted(workload["requests"], key=lambda row: (row["prompt_tokens"] + row["output_tokens"], row["id"]), reverse=True)
        for request in ordered:
            if len(reduced["requests"]) == minimum:
                break
            candidate = {**reduced, "requests": [row for row in reduced["requests"] if row["id"] != request["id"]]}
            if "ep_routes" in reduced:
                candidate["ep_routes"] = {name: {**route, "assignments": [
                    entry for entry in route["assignments"] if entry["request"] != request["id"]
                ]} for name, route in reduced["ep_routes"].items()}

            # A request can be structurally redundant and still establish a necessary measured length.
            if required <= self.conditions(instances, candidate) | self.length_evidence(candidate, bounds):
                reduced = candidate
        return reduced

    @staticmethod
    def cache_conditions(mode, configuration, content):
        page = configuration["page_size"]
        shared = {request["id"] for request in LifecycleProbes.shared_prefix_requests(content, page)}
        conditions = set()
        for request in content.requests.values():
            prompt, output = request["prompt_tokens"], request["output_tokens"]
            calls = [("cache.finished", prompt + (0 if mode == "prefill" else output - 1))]
            if mode == "null" and output > 1:
                calls.append(("cache.unfinished", prompt))
            for operation, tokens in calls:
                protected = {0}

                # Consecutive prefix probes can retain every full page while executing the final partial page.
                if operation == "cache.unfinished" and request["id"] in shared and tokens % page:
                    protected.add(tokens // page * page)
                for retained in protected:
                    guards = CacheInputs.guards(operation, {"page_size": page, "cache_tokens": tokens, "protected_tokens": retained})
                    conditions.add(operation + "/" + json.dumps(guards, separators=(",", ":")))
        return conditions

    def route_conditions(self, instance, workload):
        size = instance.configuration.ep
        if size == 1:
            return set()
        experts = self.expert_count
        if type(experts) is not int or experts <= 0 or experts % size:
            raise ValueError("expert source planning requires an even native expert partition")
        width = experts // size
        route = instance.configuration.routes(workload)
        conditions = {"padding/" + json.dumps(sorted(json.dumps(entry, sort_keys=True) for entry in route.get("padding", [])))}
        requests = {request["id"]: request for request in workload["requests"]}
        for assignment in route["assignments"]:
            prompt = requests[assignment["request"]]["prompt_tokens"]
            begin, end = assignment["tokens"]
            phases = (["prefill"] if begin < prompt else []) + (["decode"] if end > prompt else [])
            receivers = {expert // width for expert in assignment["experts"]}
            for phase in phases:
                for layer in range(*assignment["layers"]):

                    # Preserve phase and joint receiver opportunities without claiming actual native batch coverage.
                    conditions.add(f"expert/{phase}/{layer}/receivers/" + ",".join(str(rank) for rank in sorted(receivers)))
        return conditions

    @staticmethod
    def model_phases(instance, conditions):
        config = instance.configuration.values
        if NativeConfiguration.batch_capacity(config) == 1:
            return set()

        mode = instance.mode
        phases = set()
        if mode != "decode" and config["tp"] > 1 and f"{mode}/multiple_prefill_inputs" in conditions:
            phases.add("prefill")
        if mode != "prefill" and f"{mode}/concurrent_decode_inputs" in conditions:
            phases.add("decode")
        return phases

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

    def select(self, candidates: list[tuple[str, str, ServingInput]], *, service_evidence=False):
        groups, instances = {}, {}
        for configuration, name, serving in candidates:
            instances[configuration] = serving.instances
            groups.setdefault((configuration, self.padding(serving)), []).append({
                "name": name, "conditions": self.conditions(serving.instances, self.workloads[name]),
                "cost": self.cost(serving.workload, serving.instances),
            })
        geometry = {}
        if service_evidence:
            for (configuration, padding), remaining in groups.items():
                lengths = [self.lengths(self.workloads[item["name"]]) for item in remaining]
                bounds = {}
                for axis in lengths[0]:
                    points = set().union(*(row[axis] for row in lengths))
                    if points:
                        bounds[axis] = {min(points), max(points)}
                geometry[configuration, padding] = bounds
                for item in remaining:
                    item["conditions"].update(self.length_evidence(self.workloads[item["name"]], bounds))

        targets = {(configuration, item["name"]): item["conditions"]
                   for (configuration, _), remaining in groups.items() for item in remaining}
        choices = []
        for (configuration, padding), remaining in groups.items():
            bounds = geometry.get((configuration, padding), {})
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

            # A later workload can subsume an earlier one before reducing requests within the retained inputs.
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
            covered = set()
            mixed_service = service_evidence and any(
                instance.configuration.values["mixed_batches"] for instance in instances[configuration]
            )
            for item in selected:
                name, conditions = item["name"], item["conditions"]
                original = self.workloads[name]
                representative = original
                if not mixed_service:
                    representative = self.reduce_requests(instances[configuration], original, conditions, bounds)

                    # Fixed arrival gaps cannot be rescaled without knowing native execution time.
                    if "later_arrival" not in conditions:
                        shortened = probes.normalize(representative)
                        shortened_conditions = self.conditions(instances[configuration], shortened) | self.length_evidence(shortened, bounds)
                        if conditions <= shortened_conditions:
                            representative = shortened

                # Repeated requests can coexist with later chunks even when their individual lengths are redundant.
                normalized[name] = representative
                covered.update(self.conditions(instances[configuration], representative) | self.length_evidence(representative, bounds))
            workload, submissions = self.combine(names, topologies, normalized)
            phases = {instance.mode: self.model_phases(instance, covered) for instance in instances[configuration]}
            probes.extend(workload, submissions, model_phases=set().union(*phases.values()))
            requirements = {mode: sorted(f"{phase}/{size}" for phase in values for size in ("single", "multiple"))
                            for mode, values in phases.items() if values}
            cost = self.cost(workload, instances[configuration])
            cost.pop("last_arrival_ns")
            requests = {request["id"]: request for request in workload["requests"]}
            cost["arrival_wait_ns"] = sum(max(requests[name]["arrival_ns"] for name in group) for group in submissions)
            choices.append({"configuration": configuration, "workloads": names,
                            "input_cost": cost, "input_conditions": sorted(covered),
                            "required_coverage": requirements,
                            "original_tokens": sum(item["cost"]["prompt_tokens"] + item["cost"]["decode_tokens"] for item in selected),
                            "submission_group_count": len(submissions),
                            "targets": {target for target, required in targets.items() if required <= covered},
                            "workload": workload, "submission_groups": submissions})
        selected, remaining = [], set(targets)
        while remaining:

            # Each recipe covers whole declared target inputs; incompatible conditions need another planned source.
            choice = min((item for item in choices if item["targets"] & remaining), key=lambda item: (
                -len(item["targets"] & remaining), item["input_cost"]["prompt_tokens"] + item["input_cost"]["decode_tokens"],
                item["input_cost"]["arrival_wait_ns"], item["input_cost"]["worker_kv_tokens"],
                item["input_cost"]["requests"], item["configuration"], item["workloads"],
            ))
            remaining.difference_update(choice["targets"])
            selected.append(choice)
            choices.remove(choice)

        # Remove whole redundant jobs before publishing; no source is added in response to target results.
        for choice in reversed(selected.copy()):
            others = [item for item in selected if item is not choice]
            if set(targets) <= set().union(*(item["targets"] for item in others)):
                selected.remove(choice)
        result = []
        for choice in selected:
            workload, submissions, covered = choice.pop("workload"), choice.pop("submission_groups"), choice.pop("targets")
            result.append((choice, workload, submissions, covered))
        return result
