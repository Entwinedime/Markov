"""Check SGLang model index bindings and declared attention-state ordering in a target graph."""

from collections import Counter, defaultdict

from execution import DependencyPaths


class ServingOperandEvidence:
    def __init__(self, result: dict) -> None:
        self.graph = result["graph"]
        self.page_size = result["configuration"]["page_size"]
        self.models = defaultdict(list)
        self.columns = Counter()
        self.failures = Counter()
        self.examples = []
        for region in self.graph["regions"]:
            if region.get("entrypoint") in {"Prefill", "Decode", "Mixed"}:
                task = region["task"]
                self.models[task["group"], task["sequence"]].append(region)

    def require(self, valid: bool, check: str, region: dict) -> None:
        if valid:
            return
        self.failures[check] += 1
        if len(self.examples) < 8:
            self.examples.append({"check": check, "region": region["id"], "task": region["task"]})

    def observe(self, action: dict, residences: dict[str, dict]) -> None:
        task = action["task"]
        models = self.models[task["group"], task["sequence"]]
        if not models:
            return
        expected = defaultdict(list)
        for member in action["members"]:
            begin = member["context_tokens"]
            end = begin + member["new_tokens"]
            pages = residences[member["id"]]["pages"]

            # SGLang reserves physical page and request slot zero; owner records use zero-based identities.
            expected["kv_locations"].extend(
                (pages[token // self.page_size] + 1) * self.page_size + token % self.page_size
                for token in range(begin, end)
            )
            for name in ("request_slots", "request_slots_cpu"):
                expected[name].append(member["slot"] + 1)
            for name in ("sequence_lengths", "sequence_lengths_cpu", "original_lengths"):
                expected[name].append(end)

        for model in models:
            for name, contents in expected.items():
                value = self.graph["values"][model["ports"]["inputs"][name]]
                self.columns[name] += 1
                recorded = value.get("integer_data")
                if recorded is None:
                    self.require(False, name + "_contents", model)
                    continue
                actual = [index for start, count in recorded for index in range(start, start + count)]
                self.require(actual == contents, name, model)

    def check(self) -> dict:
        execution = self.graph["execution"]
        attention, owners = {}, {}
        for region in self.graph["regions"]:
            identity = region["id"]
            if region.get("type") == "sglang.srt.layers.radix_attention.RadixAttention":
                attention[identity] = region
                owners[identity] = identity
            else:
                owners[identity] = owners.get(region["parent"])

        operations, writes = {}, defaultdict(set)
        for operation in self.graph["operations"]:
            owner = owners[operation["region"]]
            if owner is not None:
                operations[operation["id"]] = owner
                writes[owner].update(operation["writes"])

        nodes = [
            node
            for node in execution["nodes"]
            if node.get("operation") in operations and node["kind"] in {"device", "transfer", "copy"}
        ]
        order = DependencyPaths.resource_order(execution, nodes)
        streams = defaultdict(lambda: defaultdict(list))
        for node in nodes:
            streams[operations[node["operation"]]][node["resource"]].append(node["id"])
        ports = {
            operation: [
                (min(nodes, key=order.__getitem__), max(nodes, key=order.__getitem__)) for nodes in groups.values()
            ]
            for operation, groups in streams.items()
        }
        previous, bindings, required = {}, {}, set()
        for identity, region in attention.items():
            task = region["task"]
            owner = task["group"], task["sequence"]
            layer = region["rank"], region["parameters"]["layer"]
            inputs = {self.graph["values"][value]["storage"] for value in region["inputs"]}

            # Coordination may write KV separately from the device call; local scratch is not persistent state.
            stores = writes[identity] & inputs
            current = ports.get(identity, [])
            self.require(bool(stores) and bool(current), "attention_state_and_device_work", region)
            self.require(bindings.setdefault(layer, stores) == stores, "stable_layer_storage", region)
            for storage in stores:
                if storage in previous:
                    source, producer = previous[storage]
                    if producer != owner:
                        required.update((left[1], right[0]) for left in source for right in current)
                previous[storage] = current, owner

        missing = DependencyPaths(execution).missing(required)
        return {
            "status": "matched" if attention and self.columns and not self.failures and not missing else "differences",
            "scope": (
                "model entry index bindings and cross-action declared attention-state device paths; "
                "not backend-internal index evaluation, KV numerical contents or PD byte-address equivalence"
            ),
            "model_columns": dict(self.columns),
            "attention_calls": len(attention),
            "layer_bindings": len(bindings),
            "required_device_paths": len(required),
            "missing_device_paths": len(missing),
            "missing_examples": sorted(missing)[:8],
            "failures": dict(self.failures),
            "examples": self.examples,
        }
