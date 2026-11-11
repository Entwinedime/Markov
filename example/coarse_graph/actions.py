"""Identify observed actions by logical work, independently of template selection."""

import json
from collections import Counter, defaultdict


class QueryResults:
    """Check reads against this execution's producers, without pairing native polling counts."""

    def __init__(self):
        self.groups, self.pending = {}, {}
        self.signals, self.signal_completions = {}, {}
        self.results = {}
        self.counts = Counter()
        self.completion_reads = 0
        self.notifications = 0
        self.notices, self.bounds = {}, {}
        self.notice_inputs = defaultdict(set)

    def observe(self, record):
        kind = record["kind"]
        if kind == "resources":
            self.groups = {group["rank"]: group["id"] for instance in record["instances"] for group in instance["groups"]}
        elif kind == "graph":
            graph = record["graph"]
            scope = graph["scopes"][0]
            for port in scope["ports"]:
                if port["use"] == "signal" and port["direction"] == "output":
                    event = record["event_ids"][port["event"]]
                    self.signals[port["signal"]] = event
                    self.signal_completions[event] = None
            if scope["role"] != "query":
                return
            event = next(port["event"] for port in scope["ports"] if port["name"] == "call.return")
            entered = next(port["event"] for port in scope["ports"] if port["name"] == "call.enter")
            rank = graph["resources"][graph["events"][event]["resource"]]["rank"]
            self.pending[record["event_ids"][event]] = {
                "kind": scope["work"]["context"]["query"], "group": self.groups[rank],
                "returned_ns": None, "entered": record["event_ids"][entered], "reads": 0, "last_read": None,
                "work": scope["work"],
            }
        elif kind == "notification_declared":
            event = record["event"]
            if event in self.signal_completions or record["identity"]["group"] not in self.groups.values():
                raise ValueError("notification has a duplicate event or unknown participant")
            self.notices[event] = record["identity"]
            self.signal_completions[event] = None
        elif kind == "boundary_time":
            self.bounds[record["event"]] = record["begin_ns"], record["end_ns"]
        elif kind == "edge" and record["target"] in self.notices:
            self.notice_inputs[record["target"]].add(record["source"])
        elif kind == "completion":
            event = record["event"]
            if event in self.bounds:
                begin, end = self.bounds.pop(event)
                if not begin <= record["time_ns"] <= end:
                    raise ValueError("completed event violates its measured time interval")
            if event in self.pending:
                self.pending[event]["returned_ns"] = record["time_ns"]
            if event in self.signal_completions:
                self.signal_completions[event] = record["time_ns"]
        elif kind == "notification_ready":
            events = record["prerequisites"]
            if (self.notices.pop(record["event"]) != record["identity"]
                    or self.signal_completions[record["event"]] != record["time_ns"] or not events or len(events) != len(set(events))
                    or set(events) != self.notice_inputs.pop(record["event"])
                    or any(self.signal_completions[event] is None or self.signal_completions[event] > record["time_ns"] for event in events)):
                raise ValueError("notification was released before its actual producers completed")
            self.notifications += 1
        elif kind == "completion_read":
            query = self.pending[record["boundary"]]
            event = self.signals[record["signal"]]
            completed_ns = self.signal_completions[event]

            # Recorded order distinguishes a read from a completion at the same simulated time.
            if (query["returned_ns"] is None or record["time_ns"] < query["returned_ns"]
                    or record["group"] != query["group"] or record["event"] != event
                    or type(record["result"]) is not bool or record["result"] != (completed_ns is not None)
                    or (completed_ns is not None and completed_ns > record["time_ns"])):
                raise ValueError("completion read disagrees with its query, signal producer or completion state")
            query["reads"] += 1
            query["last_read"] = record
            self.completion_reads += 1
            return query
        elif kind == "state_query":
            query = self.pending.pop(record["boundary"])
            expected_type = int if query["kind"] == "transport" else bool

            # A protocol poll can use cached state or multiple reads; an event query reads exactly one signal.
            last_read = query["last_read"]
            if (query["kind"] not in {"event", "transport", "metadata"} or type(record["result"]) is not expected_type
                    or record["group"] != query["group"] or query["returned_ns"] is None
                    or record["time_ns"] < query["returned_ns"] or record["completion_reads"] != query["reads"]
                    or (last_read is not None and record["time_ns"] < last_read["time_ns"])
                    or (query["kind"] == "event" and (query["reads"] != 1 or record["result"] != last_read["result"]))):
                raise ValueError("query result disagrees with its worker, result type, boundary or actual reads")
            self.results[query["group"], query["entered"]] = record["result"]
            self.counts[query["kind"]] += 1
            return query

    def finish(self):
        if self.pending or self.notices or self.bounds:
            raise ValueError("execution has unfinished query, notification or measured boundary observations")
        return dict(self.counts)


class ActionScopes:
    def __init__(self, graph):
        self.graph = graph
        self.children, self.work = defaultdict(list), defaultdict(list)
        for index, scope in enumerate(graph["scopes"]):
            self.children[scope["parent"]].append(index)
        for piece in graph["work"]:
            self.work[piece["scope"]].append(piece)
        self.placements = {}

    @staticmethod
    def expression_key(expression):
        if isinstance(expression, dict):
            return {name: ActionScopes.expression_key(value) for name, value in expression.items()}
        if not isinstance(expression, list):
            return type(expression).__name__, expression
        operation, *arguments = expression
        values = tuple(ActionScopes.expression_key(value) for value in arguments)

        # Only binary commutation preserves the DSL's checked arithmetic without changing overflow order.
        return operation, frozenset(values) if len(values) == 2 and operation in {"add", "mul", "eq"} else values

    @classmethod
    def expression_list_key(cls, expressions):
        return None if expressions is None else tuple(cls.expression_key(value) for value in expressions)

    @classmethod
    def declaration_expressions(cls, scope):
        return {"arguments": cls.expression_key(scope.get("arguments", {})),
                "guards": cls.expression_list_key(scope.get("guards", []))}

    def validate_source(self, raw, rank):
        if raw["environment"]["parallel"]["rank"] != rank or self.graph["environment"] != raw["environment"]:
            raise ValueError("graph and recording belong to different worker environments")
        self.source_ids = {}
        for index, scope in enumerate(self.graph["scopes"]):
            origins = scope["bindings"]["source_scopes"]
            if not origins or origins != sorted(set(origins)):
                raise ValueError("coarse scope needs ordered unique native origins")
            for origin in origins:
                if type(origin) is not int or not 0 <= origin < len(raw["scopes"]) or origin in self.source_ids:
                    raise ValueError("native scope has an invalid or competing coarse owner")
                self.source_ids[origin] = index
        if self.source_ids.keys() != set(range(len(raw["scopes"]))):
            raise ValueError("coarse scopes omit native observations")

        # Ordinary declarations remain exact; a closed transfer region is checked from all its native members.
        for index, scope in enumerate(self.graph["scopes"]):
            origins = scope["bindings"]["source_scopes"]
            if scope["implementation"] == "sglang.pd.transfer_group":
                self.validate_transfer_group(index, origins, raw)
                continue
            if len(origins) != 1:
                raise ValueError("ordinary coarse scope combines distinct native declarations")
            observed = raw["scopes"][origins[0]]
            bindings = dict(observed["bindings"])
            bindings.update({name: observed[name] for name in ("effects", "publication") if name in observed})
            bindings["source_scopes"] = origins
            parent = None if observed["parent"] is None else self.source_ids[observed["parent"]]
            if (not observed["complete"] or scope["bindings"] != bindings or scope["parent"] != parent
                    or scope.get("communication") != observed.get("communication")
                    or any(scope[field] != observed[field] for field in ("role", "site", "implementation", "parameters", "constants", "work"))
                    or self.declaration_expressions(scope) != self.declaration_expressions(observed)):
                raise ValueError(f"graph scope {index} differs from its native recording")

            # Check native tensor facts before either interpretation or projection can become the comparison's truth.
            ports = {(port["name"], port["direction"]): port for port in scope["ports"]}
            for port in observed["ports"]:
                tensor = port.get("tensor")
                if tensor is None:
                    continue
                boundary = ports[port["name"], port["direction"]]
                projected = boundary["tensor"]
                if (projected is None
                        or any(tensor[field] != projected[field] for field in ("dtype", "shape", "value", "storage", "version", "buffer"))
                        or projected["allocation"] != (None if tensor["allocation"] is None else self.source_ids[tensor["allocation"]])
                        or projected["writer"] != (None if tensor["writer"] is None else self.source_ids[tensor["writer"]])
                        or self.expression_list_key(tensor.get("shape_rule")) != self.expression_list_key(projected.get("shape_rule"))):
                    raise ValueError(f"graph scope {index} changes native tensor port {port['name']}/{port['direction']}")

                # A blocking host read consumes by return, regardless of the tensor's storage device.
                if port.get("at_return", False):
                    if (port["direction"] != "input" or port["use"] != "host" or boundary["use"] != "data"
                            or boundary["event"] != ports["call.return", "output"]["event"]):
                        raise ValueError(f"graph scope {index} changes completed host read {port['name']}")

            if "native_transfer" not in observed:
                self.validate_state_accesses(index, observed, scope, ports)

        # Each read retains its own observed action and result, even when its logical identity repeats.
        reads = set()
        for query in raw["state_queries"]:
            scope = query["scope"]
            if type(scope) is not int or not 0 < scope < len(raw["scopes"]) or scope in reads:
                raise ValueError("query requires its own unique observed action")
            reads.add(scope)
            observed = raw["scopes"][scope]
            kind = query["kind"]
            result_type = int if kind == "transport" else bool
            if (kind not in {"event", "transport", "metadata"} or type(query["result"]) is not result_type
                    or observed["role"] != "query" or not observed["bindings"].get("action")
                    or observed["bindings"]["condition"] != {"query": kind}
                    or observed["work"]["context"]["query"] != kind):
                raise ValueError("query observation disagrees with its action or result contract")
            if kind != "event" and not observed["work"]["context"]["request"]["id"]:
                raise ValueError("protocol and metadata queries require a logical request")
        if reads != {index for index, scope in enumerate(raw["scopes"]) if scope["role"] == "query"}:
            raise ValueError("query actions lack observed results")
        self.bind_pipeline_placements(raw)

    def validate_state_accesses(self, index, observed, scope, ports):
        for space, field in (("kv", "kv_accesses"), ("slot", "slot_accesses")):
            native = [access for access in observed.get("state_accesses", ()) if access["space"] == space]
            if len(native) != len(scope[field]):
                raise ValueError(f"graph scope {index} changes its declared {space} accesses")
            for access, projected in zip(native, scope[field], strict=True):
                expected = {key: value for key, value in access.items() if key != "interval"}
                actual = {key: value for key, value in projected.items() if key not in {"start", "finish"}}
                if self.expression_key(expected) != self.expression_key(actual):
                    raise ValueError(f"graph scope {index} changes its declared {space} access contents")
                interval = access["interval"]
                if interval == "service":
                    for side, endpoint in (("begin", "start"), ("end", "finish")):
                        event = projected[endpoint]
                        resource = self.graph["resources"][self.graph["events"][event]["resource"]]
                        if (resource["kind"] != "service" or resource["identity"] != f"service/{observed['thread']}"
                                or self.graph["evidence"]["events"][event]["order"] != observed["host_interval"][side + "_counter"]):
                            raise ValueError(f"graph scope {index} changes its declared service access interval")
                    continue
                entered, returned = {"call": ("call.enter", "call.return"), "device": ("device.enter", "device.complete")}[interval]
                begin, end = ports[entered, "input"]["event"], ports[returned, "output"]["event"]
                if (projected["start"], projected["finish"]) != (begin, end):
                    raise ValueError(f"graph scope {index} changes its declared {space} access interval")

    def bind_pipeline_placements(self, raw):
        contexts = {}
        queries = {query["scope"]: query["point"] for query in raw["state_queries"] if query["kind"] == "event"}
        for scope in raw["scopes"]:
            producer = scope["work"].get("context", {}).get("producer", {})
            if "pipeline" not in producer:
                continue
            points = [port["point"] for port in scope["ports"]
                      if port["use"] == "signal" and port["direction"] == "input" and "point" in port]
            if scope["id"] in queries:
                points.append(queries[scope["id"]])
            for point in points:
                if contexts.setdefault(point, producer["pipeline"]) != producer["pipeline"]:
                    raise ValueError("native signal consumers disagree on their producer's pipeline placement")

        # A recorded event generation carries its creator's position, independently of a later wait's current position.
        for scope in raw["scopes"]:
            if scope["role"] not in {"launch", "output_skip"}:
                continue
            for port in scope["ports"]:
                if (port["name"] == "completion" and port["direction"] == "output" and "point" in port
                        and port["point"] in contexts):
                    self.placements[self.source_ids[scope["id"]]] = contexts[port["point"]]

        # Preparation and result entry consume a PP value produced in the same native loop position.
        for index, scope in enumerate(self.graph["scopes"]):
            continuation = scope["work"].get("continuation", {})
            result_entry = (scope["role"] == "segment" and continuation.get("owner") == "result"
                            and continuation.get("entry") == "call.enter")
            if scope["role"] != "output_prepare" and not result_entry:
                continue
            for port in scope["ports"]:
                if port["name"] != "next_token_ids" or port["direction"] != "input":
                    continue
                writer = port["tensor"]["writer"]
                while writer is not None:
                    owner = self.graph["scopes"][writer]
                    placement = self.placements.get(writer, owner["work"].get("context", {}).get("pipeline"))
                    if placement is not None:
                        self.placements[index] = placement
                        if result_entry:
                            self.placements[scope["parent"]] = placement
                        break
                    writer = owner["parent"]

    def record(self, index, group):
        record = ActionIdentity.record(self.graph["scopes"][index], group)
        if record["work"]["observation_context"] is not None:
            return record

        # Nested calls inherit the envelope's entry position; their own continuation and logical progress remain separate.
        owner = index
        while owner is not None:
            if owner in self.placements:
                record["work"]["observation_context"] = {"pipeline": self.placements[owner]}
                break
            owner = self.graph["scopes"][owner]["parent"]
        return record

    def validate_transfer_group(self, index, origins, raw):
        group = self.graph["scopes"][index]
        calls = sorted((raw["scopes"][origin] for origin in origins), key=lambda call: call["host_interval"]["begin_counter"])
        parent = calls[0]["submitted_by"]
        expected = {call["id"] for call in raw["scopes"] if call.get("submitted_by") == parent and "native_transfer" in call}
        if set(origins) != expected or any(
            not call["complete"] or call["parent"] != parent or call["thread"] != calls[0]["thread"]
            or any(call[field] != group[field] for field in ("parameters", "constants"))
            or self.declaration_expressions(call) != self.declaration_expressions(group)
            or call["ports"] or call.get("communication") for call in calls
        ):
            raise ValueError("coarse transfer group differs from its complete native service cohort")
        work = {}
        for call in calls:
            for key, value in call["work"].items():
                if key in work and work[key] != value:
                    raise ValueError("native services disagree on request context")
                work[key] = value
        if group["work"] != work:
            raise ValueError("coarse transfer group changes its logical work")
        begin, end = calls[0]["host_interval"]["begin_raw_ns"], calls[-1]["host_interval"]["end_raw_ns"]
        if (group["role"] != "kv_transfer" or group["site"] != "transfer_group"
                or group["parent"] != self.source_ids[parent] or self.children[index]
                or any(a["host_interval"]["end_counter"] > b["host_interval"]["begin_counter"] for a, b in zip(calls, calls[1:]))):
            raise ValueError("coarse transfer group loses native caller structure or serial order")
        if any(call["id"] not in origins and call["thread"] == calls[0]["thread"]
               and call["host_interval"]["begin_raw_ns"] < end and begin < call["host_interval"]["end_raw_ns"]
               for call in raw["scopes"][1:]):
            raise ValueError("coarse transfer group hides another native invocation")
        ports = {port["name"]: port for port in group["ports"]}
        if ports.keys() != {"call.enter", "call.return", "service.complete"}:
            raise ValueError("closed transfer group has extra or missing public boundaries")
        for name, time in (("call.enter", begin), ("call.return", end), ("service.complete", end)):
            if self.graph["evidence"]["events"][ports[name]["event"]]["time_ns"] != time:
                raise ValueError("coarse transfer group changes its native outer interval")
        work, = self.work[index]
        if work["kind"] != "transfer" or self.graph["resources"][work["resource"]]["kind"] != "service":
            raise ValueError("coarse transfer group duplicates CPU cost or loses transport work")
        if any(access["start"] != work["start"] or access["finish"] != work["finish"]
               for field in ("kv_accesses", "slot_accesses") for access in group[field]):
            raise ValueError("coarse transfer payload escapes its closed service interval")

class ActionIdentity:
    @classmethod
    def request_progress(cls, value):
        if isinstance(value, list):
            return any(cls.request_progress(item) for item in value)
        if not isinstance(value, dict):
            return False

        # Request progress follows the shared ABI; its presence in a snapshot does not establish work ownership.
        return {"id", "generated_tokens", "committed_tokens", "extend"} <= value.keys() or any(
            cls.request_progress(item) for item in value.values()
        )

    @classmethod
    def request_work(cls, record):
        work = record["work"]

        # Queue/caller snapshots observe requests; only this call's declared request or batch operands own work.
        return bool(work["members"]) or any(
            cls.request_progress(work["context"].get(operand)) for operand in ("request", "members", "left", "right")
        )

    @classmethod
    def occurrence_anchored(cls, record):

        # A readiness read can recur for the same work; one observation does not make it a unique occurrence.
        return record["action"] != "query" and cls.request_work(record)

    @staticmethod
    def selected(index, scope):
        return bool(index) and (scope["role"] == "segment" or scope["bindings"].get("action", False)
                                or (scope["parent"] == 0 and scope["role"] != "invocation"))

    @staticmethod
    def record(scope, group):
        work = scope["work"]

        # Preserve submitted token ranges; later request progress can be shared by different in-flight batches.
        logical = {
            "members": [{name: value for name, value in member.items() if name != "slot"}
                        for member in work.get("members", ())],
            "context": work.get("context"), "callers": work.get("callers", []),
            "continuation": work.get("continuation"), "observation_context": work.get("observation_context"),
        }
        if not isinstance(logical["context"], dict) or not isinstance(logical["callers"], list):
            raise ValueError(f"{scope['role']} has no valid logical work/caller context")
        if logical["continuation"] is not None and not isinstance(logical["continuation"], dict):
            raise ValueError(f"{scope['role']} has invalid continuation context")
        if logical["observation_context"] is not None and not isinstance(logical["observation_context"], dict):
            raise ValueError(f"{scope['role']} has invalid observation context")

        return {"group": group, "action": scope["role"], "parameters": scope["parameters"], "work": logical}

    @staticmethod
    def timing_key(record):
        return json.dumps(record, sort_keys=True)

    @staticmethod
    def logical_key(record):
        if "work" not in record:
            return json.dumps(record, sort_keys=True)

        # Placement qualifies absolute clocks; it cannot distinguish repeated logical work across runs.
        work = {name: value for name, value in record["work"].items() if name != "observation_context"}
        return json.dumps({**record, "work": work}, sort_keys=True)

    @staticmethod
    def structure_key(record):
        if "work" not in record:
            return json.dumps(record, sort_keys=True)

        # Shared structure ignores caller state; logical pairing retains it and only absolute timing keeps placement.
        work = record["work"]
        callers = [{"owner": caller["owner"], "entry": caller["entry"]} for caller in work["callers"]]
        return json.dumps({**record, "work": {
            "members": work["members"], "context": work["context"], "callers": callers, "continuation": work["continuation"],
        }}, sort_keys=True)

    @staticmethod
    def group(graph, instance):
        port = next(port for port in graph["scopes"][0]["ports"] if port["name"] == "call.enter")
        resource = graph["resources"][graph["events"][port["event"]]["resource"]]
        return f"{instance}/{resource['rank']}"
