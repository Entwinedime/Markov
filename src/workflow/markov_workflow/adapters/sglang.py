"""Assemble native participants from source-authorized roles, without importing SGLang."""

from __future__ import annotations

from markov_adapters.sglang.configuration import InstanceConfiguration, NativeConfiguration, ServingInput


class SourceDomain:
    """Match a complete native job before selecting its representative worker libraries."""

    @staticmethod
    def image_revision(image: dict) -> str:
        labels = image["Config"]["Labels"] or {}
        return NativeConfiguration.upstream_revision(labels.get("org.markov.sglang.revision"))

    @staticmethod
    def require_control(image: dict, libraries: list[dict]) -> None:
        revision = SourceDomain.image_revision(image)
        if any(library["domain"]["implementation"]["framework_revision"] != revision for library in libraries):
            raise ValueError("control image and primary source use different upstream SGLang revisions")

    @staticmethod
    def requested(serving: ServingInput) -> dict:
        return {
            "model": serving.model,
            "instances": {
                instance.mode: instance.configuration.structure() for instance in serving.instances
            },
        }

    @staticmethod
    def observed(libraries: list[dict]) -> dict:
        result = {"instances": {}}
        for library in libraries:
            domain = library["domain"]
            if domain["framework"] != "sglang":
                raise ValueError("source production has no SGLang implementation domain")
            implementation, parallel = domain["implementation"], domain["parallel"]
            model = {
                "path": implementation["model"]["_name_or_path"],
                "dtype": implementation["dtype"].removeprefix("torch."),
            }
            if result.setdefault("model", model) != model:
                raise ValueError("source production contains different models")
            structure = {
                "tp": parallel["logical_tp"], "pp": parallel["logical_pp"], "ep": parallel["logical_ep"],
                "attention_backend": implementation["attention_backend"], "page_size": implementation["page_size"],
                "prefix_cache": not implementation["cache"]["disabled"], **implementation["scheduling"],
            }
            mode = implementation["disaggregation_mode"]
            if result["instances"].setdefault(mode, structure) != structure:
                raise ValueError("source representatives disagree about their instance structure")
        return result

    @staticmethod
    def require_coverage(libraries, coverage):
        for library in libraries:
            mode = library["domain"]["implementation"]["disaggregation_mode"]
            required = coverage.get(mode)
            if not required:
                continue
            observed = {entry["condition"]["transformation"]["effect"] for entry in library["entries"]
                        if entry["action"] == "batch_transform" and entry["observations"]}

            # PP launch exposes model geometry; padding alone cannot establish concurrent requests.
            for entry in library["entries"]:
                if entry["action"] not in {"run_batch", "launch"}:
                    continue
                phase = entry["condition"]["phase"]
                for parameters in entry["observations"]:
                    if parameters["members"] > 1:
                        observed.add(f"{phase}/multiple")
                    elif parameters["members"] == parameters["model_members"] == 1:
                        observed.add(f"{phase}/single")
            missing = set(required) - observed
            if missing:
                rank = library["resources"][0]["rank"]
                raise ValueError(f"planned {mode} source rank {rank} did not observe {sorted(missing)}")


class SGLangTask:
    role_fields = ("parallel", "kv_layout", "expert_layers", "kv_peers")

    def __init__(self, production: list[dict], local_productions: list[list[dict]]) -> None:
        self.production = production
        self.libraries = [*production, *(library for local in local_productions for library in local)]

    def participants(self, selected: range, description: InstanceConfiguration) -> list[dict]:
        candidates = [
            (index, self.libraries[index]) for index in selected
            if self.libraries[index]["domain"]["implementation"]["disaggregation_mode"] == description.mode
        ]
        domain = candidates[0][1]["domain"]
        common = {name: value for name, value in domain.items() if name not in self.role_fields}
        representatives = {}
        for index, library in candidates:
            if {name: value for name, value in library["domain"].items() if name not in self.role_fields} != common:
                raise ValueError("candidate libraries describe different native role plans or implementation environments")
            rank = library["resources"][0]["rank"]
            if rank in representatives:
                raise ValueError(f"more than one library claims representative worker {rank}")
            representatives[rank] = index

        roles = sorted(domain["worker_roles"].values(), key=lambda role: (role["parallel"]["pp_rank"], role["parallel"]["tp_rank"]))
        participants = []
        for role in roles:
            rank = role["parallel"]["rank"]
            representative = role["representative"]
            if representative not in representatives:
                raise ValueError(f"worker {rank} requires missing representative library {representative}")
            index = representatives[representative]

            # Assembly only reads these facts; each worker replaces top-level bindings without cloning shared trees.
            environment = self.libraries[index]["domain"] | {name: role[name] for name in self.role_fields if name in role}
            participants.append({
                "id": f"{description.identity}/{rank}", "rank": rank, "template": index, "environment": environment,
            })
        return participants

    def document(self, source: dict) -> dict:
        if source.keys() != {"model", "instances", "workload"}:
            raise ValueError("prediction requires model, instances and workload")
        serving = ServingInput(source)
        requested = SourceDomain.requested(serving)
        if SourceDomain.observed(self.production) != requested:
            raise ValueError("primary production does not match the complete target serving domain")

        # Only the primary job supplies workers; local implementations never replace PD peers or role bindings.
        selected = range(len(self.production))
        instances = []
        for description in serving.instances:
            configuration = description.configuration
            instances.append({
                "id": description.identity, "mode": description.mode, "settings": configuration.settings(),
                "control": configuration.control(), "expert_routes": configuration.routes(serving.workload),
                "participants": self.participants(selected, description),
            })
        return {
            "instances": instances, "workload": serving.workload,
            "template_sources": list(range(len(self.libraries))),
        }
