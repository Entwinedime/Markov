"""Select SGLang source domains, representative inputs and finite local coverage."""

from markov_adapters.sglang.configuration import ServingInput
from markov_workflow.adapters.sglang import SourceDomain
from markov_workflow.adapters.sglang_local_sources import LocalSources
from markov_workflow.adapters.sglang_sources import RepresentativeSources


class SourcePlanning:
    def __init__(self, source):
        if source.keys() != {"framework", "model", "configurations", "workloads"}:
            raise ValueError("source planning needs framework, model, configurations and workloads")
        if source["framework"] != "sglang":
            raise ValueError("source planning currently requires the SGLang adapter")
        self.source = source
        self.configurations = self.named(source["configurations"], "configurations")
        self.workloads = self.named(source["workloads"], "workloads")
        self.workloads = {identity: ServingInput.workload_input(workload) for identity, workload in self.workloads.items()}
        self.representatives = RepresentativeSources(self.workloads)
        self.capture_inputs = {}
        for configuration in self.configurations.values():
            if configuration.keys() != {"id", "instances"}:
                raise ValueError("an exploration configuration needs id and complete serving instances")

    @staticmethod
    def named(values, kind):
        if not isinstance(values, list) or not values:
            raise ValueError(f"{kind} must be a nonempty array")
        result = {}
        for value in values:
            identity = value["id"]
            if not isinstance(identity, str) or not identity or identity in result:
                raise ValueError(f"{kind} IDs must be unique nonempty strings")
            result[identity] = value
        return result

    def document(self):
        domains = []
        candidates = []
        assignments = {}
        for identity, configuration in self.configurations.items():
            inputs = [(identity, name, ServingInput({
                "model": self.source["model"], "instances": configuration["instances"], "workload": workload,
            })) for name, workload in self.workloads.items()]
            serving = inputs[0][2]
            requested = SourceDomain.requested(serving)
            domain = next((item for item in domains if item["structure"] == requested), None)
            if domain is None:
                domain = {
                    "id": len(domains), "structure": requested, "configurations": [], "required_local_coverage": [],
                    "native_workers": sum(instance.configuration.tp * instance.configuration.pp for instance in serving.instances),
                }
                domains.append(domain)
                candidates.append([])
            domain["configurations"].append(identity)
            candidates[domain["id"]].extend(inputs)
            assignments[identity] = domain["id"]

        for domain, inputs in zip(domains, candidates, strict=True):
            domain["source"], workload, groups = self.representatives.select(inputs)
            domain["source"]["input"] = f"sources/{domain['id']}.json"
            self.capture_inputs[domain["id"]] = {
                "model": self.source["model"],
                "instances": self.configurations[domain["source"]["configuration"]]["instances"],
                "workload": workload, "submission_groups": groups, "observe_all_workers": False,
            }

        LocalSources(self.source["model"], self.workloads, self.configurations).extend(domains, self.capture_inputs)

        # The Cartesian target set stays factored; auxiliary sources do not become target configurations.
        return {
            "framework": self.source["framework"], "domains": domains,
            "targets": {"configurations": assignments, "workloads": list(self.workloads),
                        "count": len(assignments) * len(self.workloads)},
            "qualification": "initial_representative_plan",
            "native_jobs": len(domains),
            "limits": ["input conditions are opportunities, not observed native branches",
                       "selected workloads run sequentially after all-worker native idle; cache persists between groups",
                       "probe lengths preserve prefix, page/chunk remainders and route transitions; padding is unchanged",
                       "fixed arrival gaps keep original lengths; other shortening must preserve declared input conditions",
                       "allocator path opportunities use a cold-input page bound, not predicted batches or branch coverage",
                       "cached KV pressure and PD page reuse retain role-local demand; sharing/admission still decide eviction or sorting",
                       "shared-prefix probes run consecutively after completion; actual cache hits still require native evidence",
                       "expert-route variation outside selected workloads is not covered",
                       "ordinary and PD local coverage is checked on actual observations; missing batch/model coverage stops acquisition without supplementation",
                       "shorter probes can change admission, cache and batching; lifecycle coverage still needs native evidence"],
        }


