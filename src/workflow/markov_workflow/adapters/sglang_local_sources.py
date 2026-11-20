"""Plan a finite set of sources for closed native batch operations."""

from markov_adapters.sglang.configuration import NativeConfiguration, ServingInput
from markov_workflow.adapters.sglang import SourceDomain
from markov_workflow.adapters.sglang_probes import LifecycleProbes
from markov_workflow.adapters.sglang_sources import RepresentativeSources


class LocalSources:
    def __init__(self, model, workloads, configurations):
        self.model = model
        self.workloads = workloads
        self.configurations = configurations

    def extend(self, jobs, captures):
        if not any(len(workload["requests"]) > 1 and any(request["output_tokens"] > 1 for request in workload["requests"])
                   for workload in self.workloads.values()):
            return

        groups = {}
        for target in jobs:
            for mode, implementation in target["structure"]["instances"].items():
                configurations = [instance["configuration"] for name in target["configurations"]
                                  for instance in self.configurations[name]["instances"] if instance["mode"] == mode]
                if not any(NativeConfiguration.batch_capacity(config) > 1 for config in configurations):
                    continue

                # Mixed operations own nested work; only ordinary closed batch arrays share across shard layouts.
                independent = set() if implementation["mixed_batches"] else {"tp", "pp", "ep"}
                key = tuple((name, value) for name, value in sorted(implementation.items()) if name not in independent)
                instance = next(item for item in captures[target["id"]]["instances"] if item["mode"] == mode)
                workers = implementation["tp"] * implementation["pp"]
                group = groups.setdefault(key, {"instance": instance, "workers": workers, "consumers": []})
                if workers < group["workers"]:
                    group.update(instance=instance, workers=workers)
                group["consumers"].append({"source": target["id"], "mode": mode})

        for group in groups.values():
            identity = len(jobs)
            instance = group["instance"]
            effect = "mix" if instance["configuration"].get("mixed_batches", False) else "merge"
            required = [effect, "subset"]
            capture, selected = self.probe(instance, required, identity)
            jobs.append({"id": identity, "structure": SourceDomain.requested(ServingInput(capture)), "configurations": [],
                         "native_workers": group["workers"], "source": selected, "required_coverage": {"null": required},
                         "local_consumers": group["consumers"]})
            captures[identity] = capture

    def probe(self, instance, required, identity):
        configuration = dict(instance["configuration"])
        stages = configuration.get("pp", 1)
        configuration.update(max_running_requests=max(configuration["max_running_requests"], 3 * stages), microbatch_requests=3)
        instances = [{"id": "serving", "mode": "null", "configuration": configuration}]
        topology = configuration.get("ep")
        topologies = [] if topology is None else [topology["id"]]
        name, request = min(
            ((name, request) for name, workload in self.workloads.items() for request in workload["requests"]
             if request["output_tokens"] > 1 and all(key in workload.get("ep_routes", {}) for key in topologies)),
            key=lambda item: (item[1]["prompt_tokens"], item[1]["output_tokens"], item[0], item[1]["id"]),
        )
        original = self.workloads[name]
        seed = {**original, "requests": [request]}
        if topologies:
            seed["ep_routes"] = {key: {**original["ep_routes"][key], "assignments": [
                entry for entry in original["ep_routes"][key]["assignments"] if entry["request"] == request["id"]
            ]} for key in topologies}
        serving = ServingInput({"model": self.model, "instances": instances, "workload": seed})
        probes = LifecycleProbes(serving.instances, topologies)
        normalized = probes.normalize(seed)
        request = dict(normalized["requests"][0])
        request.pop("prefix_id", None)
        page = configuration["page_size"]
        if serving.instances[0].configuration.values["prefix_cache"] and 1 < page <= request["prompt_tokens"]:

            # Existing output1/2 pairs finish immediately below/at a whole page without extra requests.
            request["prompt_tokens"] = page - 1
            normalized = probes.resize(normalized, {request["id"]: request})
        chunk = serving.instances[0].configuration.values["prefill_chunk_tokens"]
        paged_tokens = (request["prompt_tokens"] + page - 1) // page * page

        # Cached admission rejects equality before adding; ignore-EOS admission stops at zero after adding.
        configuration["max_prefill_tokens"] = 2 * paged_tokens + int(configuration.get("prefix_cache", False))
        if chunk:

            # Both unequal-output requests must finish Prefill together before filtering their shared batch.
            configuration["prefill_chunk_tokens"] = max(chunk, 2 * paged_tokens)

        # Each PP slot receives two pairs; unequal outputs also exercise survivor filtering and merging.
        workload = {**normalized, "id": "local-operations", "prefixes": [], "requests": [
            {**request, "id": f"batch-{index}", "arrival_ns": 0,
             "output_tokens": 1 + index % 2}
            for index in range(4 * stages)
        ]}
        groups = [[member["id"] for member in workload["requests"]]]
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
                    "submission_group_count": len(groups)}
        return {"model": self.model, "instances": instances, "workload": workload,
                "observe_all_workers": False, "submission_groups": groups}, selected
