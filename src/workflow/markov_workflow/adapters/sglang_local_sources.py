"""Declare local coverage and share ordinary-serving witnesses across PD participants."""

from markov_adapters.sglang.configuration import NativeConfiguration, ServingInput
from markov_workflow.adapters.sglang import SourceDomain
from markov_workflow.adapters.sglang_probes import LifecycleProbes
from markov_workflow.adapters.sglang_sources import RepresentativeSources


class LocalSources:
    def __init__(self, model, workloads, configurations):
        self.model = model
        self.workloads = workloads
        self.configurations = configurations

    def extend(self, domains, captures):
        needed = []
        multiple = [workload for workload in self.workloads.values() if len(workload["requests"]) > 1]
        outputs = max((request["output_tokens"] for workload in multiple for request in workload["requests"]), default=1)
        for target in list(domains):
            for mode, implementation in target["structure"]["instances"].items():
                configurations = [instance["configuration"] for name in target["configurations"]
                                  for instance in self.configurations[name]["instances"] if instance["mode"] == mode]
                slots = max(NativeConfiguration.batch_capacity(config) for config in configurations)
                if not multiple or slots == 1:
                    continue
                required = set().union(*(RepresentativeSources.model_coverage(mode, config, target["source"]["input_conditions"])
                                         for config in configurations))

                if mode == "null":
                    target["required_local_coverage"] = sorted(required)
                    continue
                if not required and outputs == 1:
                    continue

                # Collect both roles before choosing a recipe; equal implementation domains need one ordinary job.
                structure = {"model": target["structure"]["model"], "instances": {"null": implementation}}
                need = next((item for item in needed if item["structure"] == structure), None)
                if need is None:
                    instance = next(item for item in captures[target["id"]]["instances"] if item["mode"] == mode)
                    need = {"structure": structure, "instance": instance, "required": set(), "consumers": []}
                    needed.append(need)
                need["required"].update(required)
                need["consumers"].append({"domain": target["id"], "mode": mode})

        if outputs > 1:
            shared = {}
            for need in needed:
                implementation = need["structure"]["instances"]["null"]
                key = tuple((name, value) for name, value in sorted(implementation.items()) if name not in {"tp", "pp", "ep"})
                shared.setdefault(key, []).append(need)
            for group in shared.values():

                # Both PD roles need batch arrays; one existing model witness can supply them across shard layouts.
                donor = min(group, key=lambda item: (
                    not item["required"],
                    item["structure"]["instances"]["null"]["tp"] * item["structure"]["instances"]["null"]["pp"],
                ))
                donor["required"].update(("merge", "subset"))
                donor["consumers"].extend(consumer for item in group if item is not donor for consumer in item["consumers"])

        for need in needed:
            if not need["required"]:
                continue

            # Matching topology alone cannot provide batch operations that the selected workload never planned to observe.
            donor = next((domain for domain in domains if domain["structure"] == need["structure"]
                          and need["required"] <= set(domain["required_local_coverage"])), None)
            if donor is None:
                capture, selected = self.probe(need["instance"], need["required"], len(domains))
                serving = ServingInput(capture)
                donor = {"id": len(domains), "structure": SourceDomain.requested(serving), "configurations": [],
                         "native_workers": serving.instances[0].configuration.tp * serving.instances[0].configuration.pp,
                         "source": selected, "required_local_coverage": []}
                domains.append(donor)
                captures[donor["id"]] = capture
            donor["local_consumers"] = need["consumers"]
            donor["required_local_coverage"] = sorted(set(donor["required_local_coverage"]) | need["required"])

    def probe(self, instance, required, identity):
        configuration = dict(instance["configuration"])
        stages = configuration.get("pp", 1)
        configuration.update(max_running_requests=max(configuration["max_running_requests"], 3 * stages), microbatch_requests=3)
        instances = [{"id": "serving", "mode": "null", "configuration": configuration}]
        topology = configuration.get("ep")
        topologies = [] if topology is None else [topology["id"]]
        outputs = 2 if "merge" in required else 1
        name, request = min(
            ((name, request) for name, workload in self.workloads.items() for request in workload["requests"]
             if request["output_tokens"] >= outputs),
            key=lambda item: (item[1]["prompt_tokens"], item[1]["output_tokens"], item[0], item[1]["id"]),
        )
        original = self.workloads[name]
        seed = {**original, "requests": [request]}
        if topologies:
            seed["ep_routes"] = {key: {**original["ep_routes"][key], "assignments": [
                entry for entry in original["ep_routes"][key]["assignments"] if entry["request"] == request["id"]
            ]} for key in topologies}
        serving = ServingInput({"model": self.model, "instances": instances, "workload": seed})
        normalized = LifecycleProbes(serving.instances, topologies).normalize(seed)
        request = normalized["requests"][0]
        request.pop("prefix_id", None)
        page = configuration["page_size"]
        paged_tokens = (request["prompt_tokens"] + page - 1) // page * page

        # Cached admission rejects equality before adding; ignore-EOS admission stops at zero after adding.
        configuration["max_prefill_tokens"] = 2 * paged_tokens + int(configuration.get("prefix_cache", False))

        # Each PP slot receives two pairs; unequal outputs also exercise survivor filtering and merging.
        workload = {**normalized, "id": "local-operations", "prefixes": [], "requests": [
            {**request, "id": f"batch-{index}", "arrival_ns": 0,
             "output_tokens": 1 + index % outputs}
            for index in range(4 * stages)
        ]}
        groups = [[member["id"] for member in workload["requests"]]]
        if "single_model_batch" in required:

            # The separate singleton supplies the degenerate layout without depending on Decode survivor timing.
            workload["requests"].insert(0, {**request, "id": "single", "arrival_ns": 0, "output_tokens": 1})
            groups.insert(0, ["single"])
        for topology in topologies:
            route = normalized["ep_routes"][topology]
            assignments = []
            for member in workload["requests"]:
                limit = member["prompt_tokens"] + member["output_tokens"] - 1
                for entry in route["assignments"]:
                    begin, end = entry["tokens"]
                    if begin < limit:
                        assignments.append({**entry, "request": member["id"], "tokens": [begin, min(end, limit)]})
            workload["ep_routes"][topology] = {**route, "assignments": assignments}

        cost = RepresentativeSources.cost(workload, serving.instances)
        cost.pop("last_arrival_ns")
        cost["arrival_wait_ns"] = 0
        selected = {"configuration": f"local-source-{identity}", "workloads": [name], "input": f"sources/{identity}.json",
                    "input_cost": cost, "input_conditions": sorted(required),
                    "original_tokens": sum(item["prompt_tokens"] + item["output_tokens"] - 1 for item in original["requests"]),
                    "submission_group_count": len(groups), "unrepresented_conditions": []}
        return {"model": self.model, "instances": instances, "workload": workload,
                "observe_all_workers": False, "submission_groups": groups}, selected

    @staticmethod
    def check(libraries, required):
        if not required:
            return
        for library in libraries:
            observed = {entry["condition"]["transformation"]["effect"] for entry in library["entries"]
                        if entry["action"] == "batch_transform" and entry["observations"]}

            # PP launch owns the run_batch child and exposes the same observed model geometry.
            members = {parameters["model_members"] for entry in library["entries"]
                       if entry["action"] in {"run_batch", "launch"} for parameters in entry["observations"]}
            if 1 in members:
                observed.add("single_model_batch")
            if any(count > 1 for count in members):
                observed.add("multiple_model_batch")
            missing = set(required) - observed
            if missing:
                rank = library["resources"][0]["rank"]
                raise ValueError(f"planned local source rank {rank} did not observe {sorted(missing)}")
