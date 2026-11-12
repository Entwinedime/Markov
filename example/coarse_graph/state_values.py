"""Compare actual control results and PD payloads, independently of template contents."""

from collections import Counter, defaultdict

from actions import ActionIdentity


class StateValueComparison:
    def __init__(self, groups):
        self.groups = groups
        self.native = defaultdict(list)
        self.predicted = defaultdict(list)
        self.returns = {}
        self.continuations = {}
        self.findings = []
        self.counts = {}

    @staticmethod
    def declared(scopes, pool):
        return {(access["field"], access["access"], request)
                for scope in scopes for access in scope if access["pool"] == pool for request in access["requests"]}

    @staticmethod
    def add(destination, group, values, declared):
        for value in values:
            field, operation, request = value["field"], value["operation"], value["request"]
            if field != "metadata" or operation not in {"write", "read"} or (field, operation, request) not in declared:
                raise ValueError("state values have no corresponding declared request access")
            payload = value["values"]
            fields = {"output_token", "cached_tokens", "cached_tokens_device", "cached_tokens_host", "cached_tokens_storage"}
            if payload is not None and (payload.keys() != fields or any(type(item) is not int or item < 0 for item in payload.values())):
                raise ValueError("PD payload evidence lacks native token/cache scalar values")
            if type(value["room"]) is not int or value["room"] <= 0:
                raise ValueError("PD payload evidence lacks its native request room")
            destination[group, request, operation].append({"room": value["room"], "values": payload})

    def source(self, raw, group):
        values = {}
        for record in raw.get("action_values", ()):
            if "state" not in record["values"]:
                continue
            scope = record["scope"]
            if type(scope) is not int or not 0 < scope < len(raw["scopes"]) or scope in values:
                raise ValueError("state value observation has no unique native scope")
            values[scope] = record["values"]["state"]

        declarations = {raw["scopes"][index]["parent"]: set()
                        for index in values if raw["scopes"][index]["role"] == "segment"}
        if any(raw["scopes"][parent]["role"] != "invocation" for parent in declarations):
            raise ValueError("state segment has no native invocation envelope")
        for index, scope in enumerate(raw["scopes"]):
            parent = scope["parent"]
            if index not in values and (scope["role"] != "segment" or parent not in declarations):
                continue
            declared = self.declared([scope.get("state_accesses", ())], "local")
            if scope["role"] == "segment" and parent in declarations:

                # A native read can precede a clock cut; its later value still belongs to this actual invocation.
                declarations[parent].update(declared)
                declared = declarations[parent]
            if index in values:
                self.add(self.native, group, values[index], declared)

    def invocation_fields(self, group, root, declared):
        work = root["work"]
        position = work.get("continuation")
        if position is None:
            return declared
        callers = work.get("callers", [])

        # Depth locates a live frame; its full work and continuation must still match across cuts.
        key = group, len(callers)
        identity = position["owner"], work.get("members", []), work["context"], callers
        if position["entry"] == "call.enter":
            if key in self.continuations:
                raise ValueError("state invocation starts before its preceding call returns")
        else:
            previous, previous_exit, previous_context, fields = self.continuations.pop(key)
            if previous != identity or (previous_exit, previous_context) != (position["entry"], position["entry_context"]):
                raise ValueError("state segments do not continue the same native invocation")
            declared = declared | fields
        if position["exit"] != "call.return":
            self.continuations[key] = identity, position["exit"], position["exit_context"], declared
        return declared

    def observe(self, row):
        if row["kind"] == "graph":
            graph = row["graph"]
            root = graph["scopes"][0]
            group = self.groups[graph["resources"][0]["rank"]]
            declared = self.declared((scope["slot_accesses"] for scope in graph["scopes"]), group)
            declared = self.invocation_fields(group, root, declared)
            if not any(field == "metadata" for field, _, _ in declared):
                return
            boundary = next(port["event"] for port in root["ports"] if port["name"] == "call.return")
            self.returns[row["event_ids"][boundary]] = {"group": group, "declared": declared, "completed": None}
        elif row["kind"] == "completion" and row["event"] in self.returns:
            self.returns[row["event"]]["completed"] = row["time_ns"]
        elif row["kind"] == "action_values" and "state" in row["values"]:
            owner = self.returns.pop(row["boundary"])
            if owner["group"] != row["group"] or owner["completed"] is None or row["time_ns"] < owner["completed"]:
                raise ValueError("state value observation precedes its own native action return")
            self.add(self.predicted, row["group"], row["values"]["state"], owner["declared"])

    def compare(self, protocol):
        requirements = protocol.requirements()
        endpoints = {(group, request, "write" if (group, request) in sources else "read")
                     for (group, request), sources in requirements.items()}
        counts = Counter()
        for origin, observed in (("native", self.native), ("predicted", self.predicted)):
            for key in observed.keys() - endpoints:
                self.findings.append(("state_payload", {"group": key[0], "request": key[1]}, {"origin": origin, "reason": "unexpected payload endpoint"}))
            for group, request, operation in endpoints:
                records = observed[group, request, operation]
                if len(records) != 1:
                    counts[f"{origin}_unpaired"] += 1
                    continue
                if records[0]["values"] is None:
                    self.findings.append(("state_payload", {"group": group, "request": request}, {"origin": origin, "reason": "native metadata consumption did not commit a token"}))

            for (group, request), senders in requirements.items():
                if (group, request) in senders:
                    continue
                received = observed[group, request, "read"]
                for sender, _ in senders:
                    written = observed[sender, request, "write"]
                    if len(written) != 1 or len(received) != 1 or written[0]["values"] is None or received[0]["values"] is None:
                        continue
                    counts[f"{origin}_transfers_checked"] += 1
                    if written[0] != received[0]:
                        self.findings.append(("state_payload", {"group": group, "request": request}, {
                            "origin": origin, "sender": sender, "written": written[0], "consumed": received[0],
                        }))
                    else:
                        counts[f"{origin}_transfers_matched"] += 1

        # Room numbers are native run-local identities; payload values are comparable under the same workload.
        for key in endpoints:
            native, predicted = self.native[key], self.predicted[key]
            if len(native) == len(predicted) == 1:
                counts["endpoints_compared"] += 1
                if native[0]["values"] != predicted[0]["values"]:
                    self.findings.append(("state_payload", {"group": key[0], "request": key[1]}, {
                        "operation": key[2], "native": native[0]["values"], "predicted": predicted[0]["values"],
                    }))
                else:
                    counts["endpoints_matched"] += 1
        self.counts = {"endpoints": len(endpoints), **counts,
                       "scope": "Native first-token and cache counts at set_buf input and successful commit output. Checks declared field ownership and action returns. Unused padding, multimodal/logprob/speculative fields and unpaired occurrences are not covered."}
        return self.findings


class ActionResults:
    """Native progress and user publication are observations, never template outputs."""

    def __init__(self, groups):
        self.groups = groups
        self.native, self.predicted, self.returns = {}, {}, {}
        self.requests = []
        self.counts = Counter()

    def source(self, raw, group, mode, source_ids):
        for index, scope in enumerate(raw["scopes"]):
            if not ActionIdentity.selected(index, scope):
                continue

            # Prefill's sender acknowledges transfer; Decode publishes the user's serving response.
            publication = [] if mode == "prefill" else scope.get("publication", [])
            self.native[group, source_ids[index]] = {"progress": scope.get("effects", []), "publication": publication}

    def observe(self, row):
        if row["kind"] == "resources":
            self.requests = [request["id"] for request in row["requests"]]
        elif row["kind"] == "graph":
            graph = row["graph"]
            ports = {port["name"]: port for port in graph["scopes"][0]["ports"]}
            entered, returned = (ports[name]["event"] for name in ("call.enter", "call.return"))
            group = self.groups[graph["resources"][graph["events"][returned]["resource"]]["rank"]]
            self.returns[row["event_ids"][returned]] = {
                "key": (group, row["event_ids"][entered]), "completed_ns": None,
            }
        elif row["kind"] == "completion" and row["event"] in self.returns:
            self.returns[row["event"]]["completed_ns"] = row["time_ns"]
        elif row["kind"] == "action_values" and "outputs" in row["values"]:
            owner = self.returns.pop(row["boundary"])
            if owner["key"][0] != row["group"] or owner["completed_ns"] != row["time_ns"]:
                raise ValueError("native action results do not belong to this completed return")
            result = {"progress": [], "publication": []}
            seen = set()
            for output in row["values"]["outputs"]:
                request = output["request"]
                if (type(request) is not int or not 0 <= request < len(self.requests) or request in seen
                        or type(output["generated_tokens"]) is not int or output["generated_tokens"] < 0
                        or type(output["finished"]) is not bool or type(output["emits_output"]) is not bool):
                    raise ValueError("native action result has an invalid or repeated request")
                seen.add(request)
                value = {"id": self.requests[request], "generated_tokens": output["generated_tokens"]}
                if output["emits_output"]:
                    result["publication"].append(value)
                else:
                    result["progress"].append({**value, "finished": output["finished"]})
            self.predicted[owner["key"]] = result

    def compare(self, pairs):
        findings = []
        self.counts["observed_returns"] = len(self.predicted)
        self.counts["missing_returns"] = len(self.returns)
        if self.returns:
            findings.append(("native_evidence", {}, {"missing_action_results": len(self.returns)}))
        for source, target in pairs.items():
            if target not in self.predicted:
                continue
            actual, predicted = self.native[source], self.predicted[target]
            self.counts["compared"] += 1
            self.counts["matched"] += actual == predicted
            if actual != predicted:
                findings.append(("action_results", {"group": source[0], "native_scope": source[1], "predicted_event": target[1]},
                                 {"native": actual, "predicted": predicted}))
        for origin, records, paired in (("native", self.native, pairs.keys()), ("predicted", self.predicted, pairs.values())):
            for key in records.keys() - set(paired):
                if any(records[key].values()):
                    self.counts[f"{origin}_unpaired_nonempty"] += 1
            for result in records.values():
                for kind, values in result.items():
                    self.counts[f"{origin}_{kind}"] += len(values)
        return findings
