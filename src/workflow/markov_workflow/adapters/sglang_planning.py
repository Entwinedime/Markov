"""Plan finite source jobs for implementation domains and declared input opportunities."""

import json
from pathlib import Path

from markov_adapters.sglang.configuration import NativeConfiguration, ServingInput
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
        self.capture_inputs = {}
        for configuration in self.configurations.values():
            if configuration.keys() != {"id", "instances"}:
                raise ValueError("an exploration configuration needs id and complete serving instances")
        self.expert_count = None
        if any(NativeConfiguration(instance["configuration"]).ep > 1
               for configuration in self.configurations.values() for instance in configuration["instances"]):
            metadata = json.loads((Path(source["model"]["path"]) / "config.json").read_text())
            counts = {metadata[name] for name in ("num_experts", "num_local_experts") if name in metadata}
            if len(counts) != 1:
                raise ValueError("expert source planning needs one native model expert count")
            self.expert_count, = counts
        self.representatives = RepresentativeSources(self.workloads, self.expert_count)

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

    @staticmethod
    def primary(plan, source, *, available):
        serving = ServingInput(source)
        structure = SourceDomain.requested(serving)
        opportunities = RepresentativeSources({}, plan["expert_count"])
        required = opportunities.conditions(serving.instances, serving.workload)
        candidates = [job for job in plan["sources"] if job["id"] in available
                      and job["structure"] == structure and job["configurations"]]
        if not candidates:
            return None

        # This chooses input evidence, not support: unseen targets still face the actual template contracts and guards.
        return min(candidates, key=lambda job: (
            len(required - set(job["source"]["input_conditions"])),
            job["source"]["input_cost"]["prompt_tokens"] + job["source"]["input_cost"]["decode_tokens"], job["id"],
        ))["id"]

    def document(self, *, service_evidence=False):
        domains = []
        for identity, configuration in self.configurations.items():
            inputs = [(identity, name, ServingInput({
                "model": self.source["model"], "instances": configuration["instances"], "workload": workload,
            })) for name, workload in self.workloads.items()]
            requested = SourceDomain.requested(inputs[0][2])
            domain = next((item for item in domains if item["structure"] == requested), None)
            if domain is None:
                domain = {"structure": requested, "inputs": []}
                domains.append(domain)
            domain["inputs"].extend(inputs)

        jobs = []
        for domain in domains:
            for selected, workload, groups, targets in self.representatives.select(domain["inputs"], service_evidence=service_evidence):
                identity = len(jobs)
                instances = self.configurations[selected["configuration"]]["instances"]
                required = selected.pop("required_coverage")
                selected["input"] = f"sources/{identity}.json"
                jobs.append({
                    "id": identity, "structure": domain["structure"],
                    "configurations": sorted({configuration for configuration, _ in targets}),
                    "required_coverage": required, "source": selected,
                    "native_workers": sum(instance["configuration"]["tp"] * instance["configuration"].get("pp", 1)
                                          for instance in instances),
                })
                self.capture_inputs[identity] = {
                    "model": self.source["model"], "instances": instances, "workload": workload,
                    "submission_groups": groups, "observe_all_workers": False,
                }

        LocalSources(self.source["model"], self.workloads, self.configurations).extend(jobs, self.capture_inputs)
        if service_evidence:
            for job in jobs:
                capture = self.capture_inputs[job["id"]]
                capture["warmup"] = capture["workload"]

                # Collective service needs readiness and payload evidence from every actual participant.
                capture["observe_all_workers"] = job["native_workers"] > 1
        return {
            "framework": self.source["framework"], "sources": jobs, "expert_count": self.expert_count,
            "targets": {"configurations": list(self.configurations), "workloads": list(self.workloads),
                        "count": len(self.configurations) * len(self.workloads)},
            "qualification": "initial_representative_plan", "native_jobs": len(jobs),
            "limits": ["input conditions are opportunities, not observed native branches or support gates",
                       "complementary jobs are fixed before capture; one complete job supplies target worker templates",
                       "selected workloads run sequentially after all-worker native idle; cache persists between groups",
                       "redundant whole requests are removed only while declared input opportunities remain; retained arrivals and expert routes are unchanged",
                "performance sources preserve prompt and final Decode context endpoints and retain work up to native resident batch capacity",
                       "selected mixed-batch workloads preserve their arriving requests for performance sampling; actual mixing still requires observation",
                       "representative lengths preserve prefix, page/chunk remainders and route transitions; padding is unchanged",
                       "model batch sizes are checked by serving role and phase; short pairs exercise joint Decode within each primary source",
                       "cached local output1/2 pairs shorten Prefill below one page when possible; request routes shift at the Prefill/Decode boundary",
                       "fixed arrival gaps keep original lengths; other shortening must preserve declared input conditions",
                       "allocator path opportunities use a cold-input page bound, not predicted batches or branch coverage",
                       "cached KV pressure and PD page reuse retain role-local demand; actual eviction/sorting still need evidence",
                       "cached-page demand counts shared token paths once; conditional expert routes may require more pages",
                       "shared-prefix probes run consecutively after completion; actual cache hits still require native evidence",
                       "receiver-empty/positive route opportunities come from supplied routes and model expert metadata",
                       "local coverage is checked on observations; failures stop acquisition without supplementation"],
        }
