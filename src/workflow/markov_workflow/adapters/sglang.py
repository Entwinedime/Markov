"""Assemble native participants from source-authorized roles, without importing SGLang."""

from __future__ import annotations

from markov_adapters.sglang.configuration import InstanceConfiguration, ServingInput


class SourceDomain:
    """Match a complete native job before selecting its representative worker libraries."""

    @staticmethod
    def requested(serving: ServingInput) -> dict:
        return {
            "model": {**serving.model, "dtype": serving.model["dtype"].removeprefix("torch.")},
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


class SGLangTask:
    role_fields = ("parallel", "kv_layout", "expert_layers", "kv_peers")

    def __init__(self, productions: list[list[dict]]) -> None:
        self.productions = productions
        self.libraries = [library for production in productions for library in production]

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
        if source.keys() != {"model", "instances", "workload", "given_work_ns"}:
            raise ValueError("prediction needs one model, instances, workload and given_work_ns")
        duration = source["given_work_ns"]
        if type(duration) is not int or not 0 <= duration < 1 << 63:
            raise ValueError("given_work_ns must be nonnegative signed 64-bit nanoseconds")
        serving = ServingInput(source)
        requested = SourceDomain.requested(serving)
        matches = []
        offset = 0
        for production in self.productions:
            if SourceDomain.observed(production) == requested:
                matches.append(range(offset, offset + len(production)))
            offset += len(production)
        if len(matches) != 1:
            raise ValueError(f"prediction requires one complete matching source production, found {len(matches)}")

        # PD peer duties belong to the same observed job; never splice locally matching halves from different jobs.
        selected = matches[0]
        instances = []
        for description in serving.instances:
            configuration = description.configuration
            instances.append({
                "id": description.identity, "mode": description.mode, "settings": configuration.settings(),
                "control": configuration.control(), "expert_routes": configuration.routes(serving.workload),
                "participants": self.participants(selected, description),
            })
        return {"instances": instances, "workload": serving.workload, "given_work_ns": duration}
