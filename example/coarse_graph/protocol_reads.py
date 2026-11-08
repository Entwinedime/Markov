"""Check SGLang PD request/peer prerequisites using native services and actual target reads."""

from collections import defaultdict


class PdStateEvidence:
    """Check native state visibility and transport prerequisites independently of polling counts."""

    def __init__(self):
        self.sessions, self.peer_counts, self.services = {}, {}, {}
        self.queries = defaultdict(list)
        self.states = defaultdict(list)
        self.admission_members = {}
        self.addresses, self.publications = {}, {}

    def observe(self, raw, group):
        if raw["kv_session"] is not None:
            session = raw["kv_session"]
            if session in self.sessions:
                raise ValueError("PD workers share a native session identity")
            self.sessions[session] = group
        if raw["kv_endpoint"] is not None:
            address = tuple(raw["kv_endpoint"])
            if address in self.addresses:
                raise ValueError("PD workers share a native control endpoint")
            self.addresses[address] = group
        self.peer_counts[group] = len(raw["environment"].get("kv_peers", {}))
        environment = raw["environment"]
        if environment["implementation"]["disaggregation_mode"] == "decode":

            # DecodeTransferQueue uses this GroupCoordinator's CPU group, with the same declared members.
            role = environment["worker_roles"][str(environment["parallel"]["rank"])]
            self.admission_members[group] = tuple(role["groups"]["attn_tp"]["members"])

        for update in raw["protocol_states"]:
            self.states[group, update["request"]].append(update)
        for query in raw["state_queries"]:
            if query["kind"] != "transport":
                continue
            request = raw["scopes"][query["scope"]]["work"]["context"]["request"]["id"]
            self.queries[group, request].append(query)
        for scope in raw["scopes"]:
            if "reservation_publication" in scope:
                publication = scope["reservation_publication"]
                context = scope["work"]["context"]
                key = group, publication["request"], context["reservation_peer"]
                if (key in self.publications or scope["role"] != "kv_reservation" or not scope["complete"]
                        or context["request"]["id"] != publication["request"]
                        or not 0 <= publication["begin_raw_ns"] <= publication["end_raw_ns"]):
                    raise ValueError(f"PD reservation lacks a unique completed native send at {key}")
                self.publications[key] = publication
            if scope["role"] == "metadata_collective" and any(
                caller["owner"] == "decode_receive" for caller in scope["work"].get("callers", [])
            ):
                members = tuple(scope["communication"]["members"])
                if self.admission_members[group] != members:
                    raise ValueError(f"native Decode admission group changes within {group}")
            if "native_transfer" not in scope:
                continue
            parent = raw["scopes"][scope["submitted_by"]]
            member, = parent["work"]["members"]
            key = group, member["id"]
            service = self.services.setdefault(key, {"end_ns": 0, "targets": set(), "metadata": set()})
            interval = scope["host_interval"]
            if not scope["complete"] or interval["begin_raw_ns"] > interval["end_raw_ns"]:
                raise ValueError(f"PD service {group}/{scope['id']} has no ordered completed interval")
            service["end_ns"] = max(service["end_ns"], interval["end_raw_ns"])
            transfer = scope["native_transfer"]
            target = transfer["session"]
            service["targets"].add(target)
            if transfer["kind"] == "metadata":
                if not parent["parameters"]["last_chunk"] or target in service["metadata"]:
                    raise ValueError(f"PD request {key} has repeated or non-final metadata")
                service["metadata"].add(target)

    def requirements(self):
        required = defaultdict(set)
        for key, service in self.services.items():
            sender, request = key
            if service["targets"] != service["metadata"] or len(service["metadata"]) != self.peer_counts[sender]:
                raise ValueError(f"PD request {key} lacks final metadata for its observed peer interfaces")
            required[key].add(key)
            for session in service["metadata"]:

                # A sender publishes Success only after its whole destination cohort has returned.
                required[self.sessions[session], request].add(key)

        return required

    @staticmethod
    def state_windows(key, records):
        windows = {}
        for record in records:
            begin, end = record["begin_raw_ns"], record["end_raw_ns"]
            before, after = record["before"], record["after"]

            # CommonKVManager takes max(old, new) for nonfailed states; snapshots may enclose concurrent writers.
            if (type(begin) is not int or type(end) is not int or not 0 <= begin <= end
                    or type(after) is not int or after not in (1, 2, 3, 4)
                    or (before is not None and (type(before) is not int or before not in (1, 2, 3, 4) or before > after))):
                raise ValueError(f"PD state write lacks an ordered nonfailed monotonic interval at {key}")
            for state in (2, 3, 4):
                if (0 if before is None else before) < state <= after:

                    # Several overlapping observations can bound the same transition, not create several transitions.
                    previous = windows.get(state, {"begin_raw_ns": begin, "end_raw_ns": end})
                    window = {"begin_raw_ns": max(begin, previous["begin_raw_ns"]),
                              "end_raw_ns": min(end, previous["end_raw_ns"])}
                    if window["begin_raw_ns"] > window["end_raw_ns"]:
                        raise ValueError(f"PD state {state} has incompatible transition intervals at {key}")
                    windows[state] = window

        # A later monotonic state cannot become visible before an earlier state's feasible interval.
        earliest = 0
        for state in sorted(windows):
            earliest = max(earliest, windows[state]["begin_raw_ns"])
            if earliest > windows[state]["end_raw_ns"]:
                raise ValueError(f"PD state transitions contradict their monotonic order at {key}")
        return windows

    @staticmethod
    def check_read(identity, result, begin, end, transitions):
        if type(result) is not int or result not in (1, 2, 3, 4) or not 0 <= begin <= end:
            raise ValueError(f"PD read has an invalid nonfailed state or interval at {identity}")

        # Reads constrain one shared monotonic transition, rather than choosing a different time for every poll.
        bounds = {state: (lower, upper) for state, lower, upper in transitions}
        feasible, earliest = [], 0
        for state in (2, 3, 4):

            # An unmeasured intermediate transition still has to satisfy every read of this endpoint.
            lower, upper = bounds.get(state, (0, None))
            if result < state:
                lower = max(lower, begin)
            else:
                upper = end if upper is None else min(upper, end)
            earliest = max(earliest, lower)
            if upper is not None and earliest > upper:
                raise ValueError(f"PD read at {identity} contradicts state {state} visibility interval")
            feasible.append((state, earliest, upper))
        return feasible

    def inspect(self):
        required = self.requirements()
        if self.states.keys() - required.keys():
            raise ValueError("PD state write has no observed request/transfer participants")
        if self.queries.keys() - required.keys():
            raise ValueError("PD query has no observed request/transfer participants")
        windows = {key: self.state_windows(key, records) for key, records in self.states.items()}
        updates = {key: values[4] for key, values in windows.items() if 4 in values}

        publications, published = [], set()
        required_publications = {
            (self.sessions[session], request, sender)
            for (sender, request), service in self.services.items() for session in service["metadata"]
        }
        for (publisher, request, peer), publication in self.publications.items():
            recipient = self.addresses[tuple(publication["target"])]
            edge = publisher, request, recipient
            if edge not in required_publications or edge in published:
                raise ValueError(f"PD reservation does not match one actual native transfer peer: {edge}")
            published.add(edge)
            ready = windows.get((recipient, request), {}).get(2)

            # The remote write may precede send return, but cannot precede invocation of any required send.
            if ready is not None and ready["end_raw_ns"] < publication["begin_raw_ns"]:
                raise ValueError(f"PD readiness precedes its reservation submission: {edge}")
            publications.append({"publisher": publisher, "request": request, "recipient": recipient, "peer": peer})

        endpoints = []

        # Transfers define the endpoints to check; missing reads cannot remove a required participant.
        for (group, request), sources in sorted(required.items()):
            queries = self.queries.get((group, request), ())
            service_end = max(self.services[source]["end_ns"] for source in sources)
            update = updates.get((group, request))
            if update is not None:
                if update["end_raw_ns"] < service_end:
                    raise ValueError(f"PD completion update at {group}/{request} precedes required service return")
                for source in sources:
                    if source != (group, request) and source in updates and update["end_raw_ns"] < updates[source]["begin_raw_ns"]:
                        raise ValueError(f"PD receiver completion at {group}/{request} precedes sender publication")
            successes, pending_after_service = [], 0
            feasible = [(state, window["begin_raw_ns"], window["end_raw_ns"])
                        for state, window in windows.get((group, request), {}).items()]
            for query in queries:
                begin, end = query["begin_raw_ns"], query["end_raw_ns"]
                feasible = self.check_read((group, request, query["scope"]), query["result"], begin, end, feasible)

                # SGLang's KVPoll ABI: Failed=0, Bootstrapping=1, WaitingForInput=2, Transferring=3, Success=4.
                if query["result"] == 4:
                    if end < service_end:
                        raise ValueError(f"PD Success at {group}/scope {query['scope']} precedes required service return by {service_end - end} ns")
                    successes.append(query)
                else:
                    if successes:
                        raise ValueError(f"PD endpoint {group}/{request} regresses after cached Success")
                    pending_after_service += begin > service_end
            first = min(successes, key=lambda query: query["begin_raw_ns"]) if successes else None
            endpoints.append({
                "group": group, "request": request, "required_senders": sorted(source[0] for source in sources),
                "remote_reservation": (group, request) in self.services,
                "service_end_raw_ns": service_end, "queries": len(queries), "success_reads": len(successes),
                "pending_reads_after_service": pending_after_service,
                "reservation_update": windows.get((group, request), {}).get(2),
                "completion_update": None if update is None else {
                    field: update[field] for field in ("begin_raw_ns", "end_raw_ns")
                },
                "first_success": None if first is None else {
                    field: first[field] for field in ("scope", "begin_raw_ns", "end_raw_ns")
                },
            })
        return {
            "status": "observed_bounds_consistent" if endpoints else "not_applicable", "endpoints": endpoints,
            "reservations": publications,
            "missing_publications": [{"publisher": publisher, "request": request, "recipient": recipient}
                                     for publisher, request, recipient in sorted(required_publications - published)],
            "completion_updates_checked": sum(row["completion_update"] is not None for row in endpoints),
            "state_writes_checked": sum(len(records) for records in self.states.values()),
            "reservation_updates_checked": sum(row["reservation_update"] is not None for row in endpoints),
            "missing_reservation_updates": [{"group": row["group"], "request": row["request"]}
                                            for row in endpoints if row["reservation_update"] is None],
            "missing_completion_updates": [{"group": row["group"], "request": row["request"]}
                                           for row in endpoints if row["completion_update"] is None],
            "missing_success_reads": [{"group": row["group"], "request": row["request"]}
                                      for row in endpoints if not row["success_reads"]],
            "scope": "Native monotonic state-write intervals constrain readiness and Success reads, independently of polling counts. Overlapping observations and all reads must admit one monotonic transition sequence; overlapping or same-time reads remain unordered. Read constraints do not rewrite the reported native write intervals. Sender readiness requires remote reservations; receiver readiness means local bootstrap initialization, not transferred data. Success additionally requires actual service returns. Missing transitions remain explicit; no timestamp is inferred from a poll. Observed reservation sends must cover exactly the actual native transfer peers and precede remote readiness. Send entry is a causal lower bound, not an exact wire publication time. These bounds do not establish exact notification arrival, metadata visibility or complete protocol qualification.",
        }


class PdPollingEvidence:
    """Follow per-rank polling values through reductions and Decode admission, without pairing rounds."""

    def __init__(self, members):
        self.members = members
        self.reads, self.inputs, self.latest = {}, {}, {}
        self.metadata_pending = {}
        self.collectives = {}
        self.commits = 0
        self.returns = {}
        self.decisions = {}
        self.checked, self.unobserved = 0, 0
        self.operand_differences = 0
        self.unobserved_commits, self.commit_differences = 0, 0
        self.findings = []

    @staticmethod
    def in_gate(work):
        return any(caller["owner"] == "decode_receive" for caller in work.get("callers", []))

    @staticmethod
    def in_poll(work):
        return any(caller["owner"] == "protocol_poll" for caller in work.get("callers", []))

    @staticmethod
    def request_order(work):
        return tuple(work["context"]["poll_requests"])

    def begin(self, group, work):
        continuation = work.get("continuation", {})
        if continuation.get("entry") != "call.enter":
            return
        if continuation.get("owner") == "protocol_poll":
            self.reads[group] = {}
            self.metadata_pending[group] = set()
        if continuation.get("owner") == "decode_receive":
            self.decisions[group] = set()
            self.latest.pop(group, None)

    def read(self, group, kind, request, result, gated):
        if kind == "transport":
            self.reads[group][request] = result
            if gated and result == 4:
                self.metadata_pending[group].add(request)
        else:
            self.metadata_pending[group].remove(request)

            # Native _apply_metadata_gate downgrades Success until the metadata row has landed.
            self.reads[group][request] = 4 if result else 3

    def operands(self, group, requests):
        if self.metadata_pending[group].intersection(requests):
            raise ValueError("Decode polling operands lack required metadata reads")
        values = self.reads[group]
        return None if values is None else [values[request] for request in requests]

    def check_operands(self, group, requests, expected, values, origin, boundary):
        if values is None:
            self.reads[group] = None
            self.decisions[group] = None
            self.unobserved += 1
            return

        # A following group reduction consumes this result, not the original local poll values.
        self.reads[group] = dict(zip(requests, values["output"], strict=True))
        self.decisions[group] = {request for request, poll in self.reads[group].items() if poll == 4}
        if expected is None:
            self.unobserved += 1
            return
        self.checked += 1
        if values["input"] != expected:
            self.operand_differences += 1
            category = "native_evidence" if origin == "native" else "polling_operands"
            self.findings.append((category, {"group": group}, {
                "reason": "PD MIN operands differ from the current reads or preceding group reduction",
                "origin": origin, "boundary": boundary, "requests": requests,
                "expected": expected, "actual": values["input"],
            }))

    def commit(self, group, request, origin, boundary):
        if len(self.members[group]) == 1:
            ready = {request} if self.operands(group, (request,)) == [4] else set()
            self.reads[group].pop(request)
        else:
            ready = self.decisions[group]
        if ready is None:
            self.unobserved_commits += 1
            return
        self.commits += 1
        if request not in ready:
            self.commit_differences += 1
            self.findings.append(("native_evidence" if origin == "native" else "admission_decision", {"group": group}, {
                "reason": "Decode commit does not consume a current unconsumed successful gate result",
                "origin": origin, "boundary": boundary, "request": request,
            }))
        else:

            # A successful result admits this request once; a later receive call must obtain a fresh decision.
            ready.remove(request)

    def native(self, raw, group):
        queries = {query["scope"]: query for query in raw["state_queries"]}
        values = {record["scope"]: record["values"]["collective"]
                  for record in raw.get("action_values", ()) if "collective" in record["values"]}

        # Scope order is local to this actual control thread; it never pairs rounds across runs.
        for scope in raw["scopes"]:
            self.begin(group, scope["work"])
            query = queries.get(scope["id"]) if self.in_poll(scope["work"]) else None
            if query is not None and query["kind"] in {"transport", "metadata"}:
                self.read(group, query["kind"], scope["work"]["context"]["request"]["id"], query["result"], self.in_gate(scope["work"]))
            if scope["role"] == "metadata_collective" and self.in_poll(scope["work"]):
                requests = self.request_order(scope["work"])
                self.check_operands(group, requests, self.operands(group, requests), values.get(scope["id"]), "native", scope["id"])
            continuation = scope["work"].get("continuation", {})
            if continuation.get("owner") == "kv_commit" and continuation["entry"] == "call.enter":
                self.commit(group, scope["work"]["context"]["request"]["id"], "native", scope["id"])

    def observe(self, record, query, groups):
        kind = record["kind"]
        if kind == "state_query" and self.in_poll(query["work"]) and query["kind"] in {"transport", "metadata"}:
            self.read(query["group"], query["kind"], query["work"]["context"]["request"]["id"], record["result"], self.in_gate(query["work"]))
        elif kind == "action_values" and record["boundary"] in self.returns and "collective" in record["values"]:
            group, requests, expected = self.returns.pop(record["boundary"])
            self.check_operands(group, requests, expected, record["values"]["collective"], "predicted", record["boundary"])
        elif kind == "edge" and record["relation"] == "communication" and record["target"] in self.collectives:
            self.collectives[record["target"]]["inputs"].add(record["source"])
        elif kind == "completion" and record["event"] in self.collectives:
            self.collectives[record["event"]]["completed"] = record["time_ns"]
        elif kind == "graph":
            self.graph(record, groups)

    def graph(self, record, groups):
        graph = record["graph"]
        root = graph["scopes"][0]
        entered = next(port["event"] for port in root["ports"] if port["name"] == "call.enter")
        rank = graph["resources"][graph["events"][entered]["resource"]]["rank"]
        group = groups[rank]
        work = root["work"]
        self.begin(group, work)
        if root["role"] == "metadata_collective" and self.in_poll(work):
            requests = self.request_order(work)
            if (len(requests) != root["parameters"]["elements"]
                    or work["context"]["collective"]["reduction"] != "RedOpType.MIN"):
                raise ValueError(f"PD polling reduction differs from its request vector or MIN at {group}")
            polls = self.operands(group, requests)
            returned = next(port["event"] for port in root["ports"] if port["name"] == "call.return")
            self.returns[record["event_ids"][returned]] = group, requests, polls

            if self.in_gate(work):
                communication, members = root["communication"], self.members[group]
                if tuple(communication["members"]) != members or tuple(communication["required"]) != members:
                    raise ValueError(f"Decode admission collective differs from native participants at {group}")
                ready = {request for request, poll in zip(requests, polls, strict=True) if poll == 4}
                port, = [port for port in root["ports"] if "rendezvous" in port]
                source, completed = record["event_ids"][port["rendezvous"]], record["event_ids"][port["event"]]

                # Actual edges join Decode operands; no cross-run ordinal supplies the peers.
                self.inputs[source] = rank, requests, ready
                self.collectives[completed] = {"inputs": set(), "completed": None, "members": members, "requests": requests}
                self.latest[group] = completed
        continuation = work.get("continuation", {})
        if continuation.get("owner") != "kv_commit" or continuation["entry"] != "call.enter":
            return
        request = work["context"]["request"]["id"]
        if len(self.members[group]) > 1:
            collective = self.collectives[self.latest[group]]
            operands = [self.inputs[event] for event in collective["inputs"]]
            if (collective["completed"] is None or collective["completed"] > record["time_ns"]
                    or len(operands) != len(collective["members"]) or {rank for rank, _, _ in operands} != set(collective["members"])
                    or any(requests != collective["requests"] or request not in ready for _, requests, ready in operands)):
                raise ValueError(f"Decode commit precedes its complete successful peer gate at {group}/{request}")
        self.commit(group, request, "predicted", record["event_ids"][entered])

    def operand_counts(self):
        return {"checked": self.checked, "unobserved": self.unobserved + len(self.returns), "different": self.operand_differences}

    def commit_counts(self):
        return {"checked": self.commits, "unobserved": self.unobserved_commits, "different": self.commit_differences}

    def finish(self):
        return {"admission_collectives_observed": len(self.collectives), "admitted_requests_checked": self.commits,
                "polling_operands": self.operand_counts(), "admission_decisions": self.commit_counts()}


class PdPredictionEvidence:
    def __init__(self, source, readiness, origin_ns):
        self.required = source.requirements()
        self.targets = {key: {source.sessions[session] for session in service["metadata"]}
                        for key, service in source.services.items()}
        self.transfers, self.signals = {}, {}
        self.reservation_targets = {(row["publisher"], row["request"], row["peer"]): row["recipient"]
                                    for row in readiness["reservations"]}
        self.reservation_events = {}
        self.notifications = {}
        self.notification_edges = set()
        self.declarations, self.timed = {}, set()
        self.bounds = {
            (row["group"], row["request"], name): (row[field]["begin_raw_ns"] - origin_ns, row[field]["end_raw_ns"] - origin_ns)
            for row in readiness["endpoints"]
            for name, field in (("kv_ready", "reservation_update"), ("kv_success", "completion_update"))
            if row[field] is not None and (name != "kv_ready" or row["remote_reservation"])
        }
        self.reads = 0
        self.polls = 0
        self.successes = 0
        self.last_states = {}
        self.polling = PdPollingEvidence(source.admission_members)

    def observe(self, record, query, queries):
        self.polling.observe(record, query, queries.groups)
        if record["kind"] == "graph":
            self.graph(record, queries.groups)
        elif record["kind"] == "notification_declared":
            identity = record["identity"]
            self.declarations[record["event"]] = identity["group"], identity["work"]["request"], identity["name"]
        elif record["kind"] == "boundary_time" and record["event"] in self.declarations:
            key = self.declarations[record["event"]]
            if key not in self.bounds or (record["begin_ns"], record["end_ns"]) != self.bounds[key]:
                raise ValueError("PD timing input differs from the independently observed state-write interval")
            self.timed.add(record["event"])
        elif record["kind"] == "notification_ready":
            identity = record["identity"]
            key = identity["group"], identity["work"]["request"]
            notice = (*key, identity["name"])
            if key not in self.required or notice in self.notifications:
                raise ValueError("PD notification has an unknown or repeated endpoint identity")
            events = set(record["prerequisites"])
            if identity["name"] == "kv_ready":
                if key not in self.targets:
                    raise ValueError("remote reservation readiness belongs to a Prefill endpoint")
                expected = {self.reservation_events[publisher, key[1], key[0]] for publisher in self.targets[key]}
                if events != expected:
                    raise ValueError("PD readiness lacks the actual native reservation publishers")
            elif identity["name"] == "kv_success":
                if key in self.targets:
                    transfer = self.transfers[key]
                    if events != transfer["events"] or transfer["metadata"] != self.targets[key]:
                        raise ValueError("PD publication lacks the complete final transfer group")
                elif events != {self.notifications[(*sender, "kv_success")]["event"] for sender in self.required[key]}:
                    raise ValueError("PD receiver visibility lacks its actual native sender publications")
            else:
                raise ValueError("PD notification has an unknown protocol transition")
            if record["event"] in self.timed:
                begin, end = self.bounds[notice]
                if not begin <= record["time_ns"] <= end:
                    raise ValueError(f"PD visibility at {notice} lies outside the independently observed state-write interval")
            self.notifications[notice] = {"event": record["event"], "time_ns": record["time_ns"]}
            self.notification_edges.update((event, record["event"]) for event in events)
        elif query is not None and query["kind"] == "transport":
            request = query["work"]["context"]["request"]["id"]
            key = query["group"], request
            required = self.required[key]
            if not required:
                raise ValueError(f"PD endpoint {key} has no independent native sender evidence")

            if record["kind"] == "state_query":
                PdStateEvidence.check_read((*key, record["boundary"]), record["result"], query["returned_ns"], record["time_ns"], (
                    (state, notice["time_ns"], notice["time_ns"])
                    for name, state in (("kv_ready", 2), ("kv_success", 4))
                    if (notice := self.notifications.get((*key, name))) is not None
                ))

                # These returns are ordered within one worker, even when simulated timestamps are equal.
                if record["result"] < self.last_states.get(key, 1):
                    raise ValueError(f"PD endpoint {key} regresses from its preceding poll")
                self.last_states[key] = record["result"]
                if key in self.targets and record["result"] >= 2:
                    ready = self.notifications.get((*key, "kv_ready"))
                    if ready is None or ready["time_ns"] > record["time_ns"]:
                        raise ValueError(f"PD sender readiness at {key} precedes its complete reservation cohort")
                self.polls += 1
            if record["kind"] == "completion_read":
                producers = self.signals[record["signal"]]
                if not producers or not producers <= required:
                    raise ValueError(f"PD endpoint {key} reads completion from another request or sender: {producers}")
                self.reads += 1
            elif record["result"] == 4:
                if (*key, "kv_success") not in self.notifications or self.notifications[(*key, "kv_success")]["time_ns"] > record["time_ns"]:
                    raise ValueError(f"PD Success at {key} precedes its own visible completion")

                # Cached Success still requires the entire native cohort, not just the signals polled this time.
                for sender in required:
                    transfer = self.transfers.get(sender)
                    if transfer is None or transfer["metadata"] != self.targets[sender]:
                        raise ValueError(f"PD Success at {key} lacks final metadata for sender {sender}")
                    for event in transfer["events"]:
                        completed_ns = queries.signal_completions[event]
                        if completed_ns is None or completed_ns > record["time_ns"]:
                            raise ValueError(f"PD Success at {key} precedes required transfer event {event}")
                    if key != sender and ((*sender, "kv_success") not in self.notifications
                                          or self.notifications[(*sender, "kv_success")]["time_ns"] > record["time_ns"]):
                        raise ValueError(f"PD receiver Success at {key} precedes notification release by {sender}")
                self.successes += 1

    def graph(self, record, groups):
        graph = record["graph"]
        root = graph["scopes"][0]
        if root["role"] == "kv_reservation":
            context = root["work"]["context"]
            publisher = groups[root["work"]["rank"]]
            request = context["request"]["id"]
            recipient = self.reservation_targets[publisher, request, context["reservation_peer"]]
            key = publisher, request, recipient
            port, = [port for port in root["ports"] if port["name"] == "reservation.ready"]
            entered = next(port["event"] for port in root["ports"] if port["name"] == "call.enter")
            if (key in self.reservation_events or port["event"] != entered
                    or port["use"] != "signal" or port["direction"] != "output"):
                raise ValueError(f"PD reservation has no unique send-entry signal: {key}")
            self.reservation_events[key] = record["event_ids"][entered]
            return
        effects = defaultdict(set)
        for scope in graph["scopes"]:
            if scope["role"] != "kv_transfer":
                continue
            for access in scope["kv_accesses"]:
                if access["access"] == "write":
                    for region in access["regions"]:
                        effects[access["finish"]].add((access["pool"], region["request"], "kv"))
            for access in scope["slot_accesses"]:
                if access["access"] == "write" and access["field"] == "metadata":
                    for request in access["requests"]:
                        effects[access["finish"]].add((access["pool"], request, "metadata"))
        if not effects:
            return
        sender = groups[root["work"]["rank"]]
        exported = set()
        for port in root["ports"]:
            if port["use"] != "signal" or port["direction"] != "output" or port["event"] not in effects:
                continue
            event = record["event_ids"][port["event"]]
            producers = set()
            for target, request, kind in effects[port["event"]]:
                key = sender, request
                if target not in self.targets[key]:
                    raise ValueError(f"PD transfer {key} writes to an unobserved destination {target}")
                transfer = self.transfers.setdefault(key, {"events": set(), "metadata": set()})
                transfer["events"].add(event)
                if kind == "metadata":
                    if root["parameters"]["last_chunk"] != 1:
                        raise ValueError("PD metadata completion is not from the final chunk")
                    transfer["metadata"].add(target)
                producers.add(key)
            self.signals[port["signal"]] = producers
            exported.add(port["event"])
        if exported != effects.keys():
            raise ValueError("PD transfer effects lack exported completion evidence")

    def finish(self):
        return {
            **self.polling.finish(),
            "completion_reads_checked": self.reads, "protocol_polls_checked": self.polls, "successful_polls_checked": self.successes,
            "final_notifications_checked": sum(key[2] == "kv_success" for key in self.notifications),
            "reservation_notifications_checked": sum(key[2] == "kv_ready" for key in self.notifications),
            "reservation_submissions_checked": len(self.reservation_events),
            "notification_intervals_checked": len(self.timed),
            "successful_endpoints": sum(state == 4 for state in self.last_states.values()),
            "scope": "Prefill readiness joins the send-entry signals of its actual native reservation publishers. Success publication follows all native-cohort transfers; each receiver visibility event depends on its actual sender publications. Every transport poll is checked against visible readiness/Success transitions and nondecreasing per-worker return values. The target read interval starts after its graph return and ends at its result observation; an overlapping or same-time transition does not impose an invented time order. Intermediate transition times and exact polling occurrence times remain unqualified. Success reads and Decode admission follow their own visibility and metadata gates. Supplied notification intervals are independently checked against native writes; a feasible realization is not an exact arrival time or proof of every polling decision, payload value or query occurrence match.",
        }
