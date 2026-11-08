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

    def validate_source(self, raw, rank):
        if raw["environment"]["parallel"]["rank"] != rank or self.graph["environment"] != raw["environment"]:
            raise ValueError("graph and recording belong to different worker environments")
        if len(self.graph["scopes"]) != len(raw["scopes"]):
            raise ValueError("graph scopes differ from this native recording")

        # Projection preserves all call declarations, including state and publication effects.
        for index, (scope, observed) in enumerate(zip(self.graph["scopes"], raw["scopes"], strict=True)):
            bindings = dict(observed["bindings"])
            bindings.update({name: observed[name] for name in ("effects", "publication") if name in observed})
            if (observed["id"] != index or not observed["complete"] or scope["bindings"] != bindings
                    or scope.get("communication") != observed.get("communication")
                    or any(scope[field] != observed[field] for field in ("role", "site", "parent", "parameters", "work"))):
                raise ValueError(f"graph scope {index} differs from this native recording")

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


class ActionIdentity:
    @staticmethod
    def selected(index, scope):
        return bool(index) and (scope["role"] == "segment" or scope["bindings"].get("action", False)
                                or (scope["parent"] == 0 and scope["role"] != "invocation"))

    @staticmethod
    def record(scope, group):
        work = scope["work"]
        logical = {"context": work.get("context"), "callers": work.get("callers", []), "continuation": work.get("continuation")}
        if not isinstance(logical["context"], dict) or not isinstance(logical["callers"], list):
            raise ValueError(f"{scope['role']} has no valid logical work/caller context")
        if logical["continuation"] is not None and not isinstance(logical["continuation"], dict):
            raise ValueError(f"{scope['role']} has invalid continuation context")

        return {"group": group, "action": scope["role"], "parameters": scope["parameters"], "work": logical}

    @staticmethod
    def occurrence_key(record):
        return json.dumps(record, sort_keys=True)

    @staticmethod
    def structure_key(record):
        if "work" not in record:
            return json.dumps(record, sort_keys=True)

        # Structure follows local work; occurrence pairing and absolute timing retain ancestor state.
        work = record["work"]
        callers = [{"owner": caller["owner"], "entry": caller["entry"]} for caller in work["callers"]]
        return json.dumps({**record, "work": {**work, "callers": callers}}, sort_keys=True)

    @staticmethod
    def group(graph, instance):
        port = next(port for port in graph["scopes"][0]["ports"] if port["name"] == "call.enter")
        resource = graph["resources"][graph["events"][port["event"]]["resource"]]
        return f"{instance}/{resource['rank']}"
