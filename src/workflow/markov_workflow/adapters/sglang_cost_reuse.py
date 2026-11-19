"""Qualify measured local batch bookkeeping across SGLang worker roles."""

from collections import defaultdict

from markov_workflow.cost_requirements import WorkPoint, cost_identity


class BatchHostReuse:
    @staticmethod
    def environment(domain):
        implementation = dict(domain["implementation"])
        implementation.pop("disaggregation_mode")
        if implementation["load_format"] == "dummy":
            implementation["model"] = {key: value for key, value in implementation["model"].items()
                                       if key != "_name_or_path"}
        return cost_identity([domain["device"], domain["torch"], domain["torch_npu"], implementation])

    @staticmethod
    def local(definition, interval):
        return (definition.get("role") == "batch_transform" and definition["work"][interval]["kind"] == "host"
                and all(not definition[field] for field in ("children", "imports", "exports", "kv_accesses", "slot_accesses")))

    @classmethod
    def match(cls, demand, missing, sources):
        environments = {key: cls.environment(domain) for key, domain in demand.domains.items()
                        if domain["framework"] == "sglang"}
        needed = defaultdict(list)
        for point in missing:
            if point.domain in environments and cls.local(demand.definitions[point.definition], point.interval):
                key = WorkPoint(environments[point.domain], point.definition, point.interval, point.phase, point.axes)
                needed[key].append(point)
        if not needed:
            return []

        selected, groups, indices = set(), {}, defaultdict(dict)
        for path, source in sources.items():
            domain = source["domain"]
            if domain["framework"] != "sglang" or "qualification" not in source:
                continue
            environment = cls.environment(domain)
            native_domain = cost_identity(domain)
            definitions = {index: cost_identity(definition) for index, definition in enumerate(source["definitions"])
                           if definition.get("role") == "batch_transform"}
            for index, row in enumerate(source["samples"]):
                definition = definitions.get(row["definition"])
                if definition is None or not row["observations_ns"] or "copy" in row or "collective" in row:
                    continue
                key = WorkPoint.at(environment, definition, row["interval"], row["features"])
                for point in needed.get(key, ()):
                    if point in selected or point.domain == native_domain:
                        continue

                    # Keep the observed duration on its host interval even when the same call has device work.
                    target = groups.setdefault(point.domain, {"domain": demand.domains[point.domain],
                                                              "definitions": [], "samples": [], "unmeasured": {}})
                    position = indices[point.domain]
                    if definition not in position:
                        position[definition] = len(target["definitions"])
                        target["definitions"].append(demand.definitions[definition])
                    target["samples"].append({**row, "definition": position[definition],
                                              "observation": {"source": str(path), "row": index}})
                    selected.add(point)
        return list(groups.values())
