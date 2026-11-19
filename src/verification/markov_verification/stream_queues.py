"""Check host stream waits against independently reconstructed command acceptance and completion."""

from collections import Counter, defaultdict


class StreamSynchronization:
    def __init__(self):
        self.submissions, self.waits = defaultdict(list), defaultdict(list)
        self.commands, self.finishes = {}, {}
        self.queued, self.last = defaultdict(set), {}
        self.declared, self.activated = set(), set()
        self.expected, self.actual = set(), set()
        self.counts, self.findings = Counter(), []

    def graph(self, body, events):
        incoming, finishes = defaultdict(set), defaultdict(set)
        for edge in body["edges"]:
            if edge["kind"] == "submission":
                incoming[edge["target"]].add(edge["source"])
        for work in body["work"]:
            finishes[work["start"]].add(work["finish"])

        # Trace submission edges name acceptance, not completion or mere insertion of a future command.
        for target, parents in incoming.items():
            resource = body["resources"][body["events"][target]["resource"]]
            if resource["kind"] != "stream":
                continue
            roots, seen, pending = set(), set(), list(parents)
            while pending:
                event = pending.pop()
                if event in seen:
                    continue
                seen.add(event)
                if event in incoming:
                    pending.extend(incoming[event])
                else:
                    roots.add(events[event])
            stream = resource["rank"], resource["identity"]
            for finish in finishes.get(target, {target}):
                key = events[target], events[finish]
                self.commands[key] = stream, events[finish], set(roots)
                self.finishes[events[finish]] = stream
                for root in roots:
                    self.submissions[root].append(key)
                self.counts["declared_commands"] += 1
            self.declared.update(roots)
        for scope in body["scopes"]:
            for port in scope["ports"]:
                if "wait" not in port:
                    continue
                wait = port["wait"]
                resource = body["resources"][wait["resource"]]
                begin, finish = events[wait["begin"]], events[port["event"]]
                self.waits[begin].append(((resource["rank"], resource["identity"]), finish))
                self.declared.add(begin)
                self.counts["declared_waits"] += 1

    def activate(self, event):
        if event not in self.declared or event in self.activated:
            raise ValueError("stream boundary activation is unknown or repeated")
        self.activated.add(event)
        for key in self.submissions.pop(event, ()):
            stream, finish, roots = self.commands[key]
            roots.remove(event)
            if not roots:
                self.queued[stream].add(finish)
                del self.commands[key]
                self.counts["accepted_commands"] += 1

        # A wait joins work accepted at this boundary and the completed frontier; later enqueues are excluded.
        for stream, finish in self.waits.pop(event, ()):
            sources = self.queued[stream] | ({self.last[stream]} if stream in self.last else set())
            self.expected.update((event, source, finish) for source in sources)
            self.counts["waits_checked"] += 1
            self.counts["empty_waits"] += not sources
            self.counts["waits_with_pending_commands"] += bool(self.queued[stream])
            self.counts["waits_with_completed_frontier"] += stream in self.last

    def observe(self, record):
        kind = record["kind"]
        if kind == "graph":
            self.graph(record["graph"], record["event_ids"])
        elif kind == "activation":
            self.activate(record["event"])
        elif kind == "completion":
            event = record["event"]
            if event in self.finishes:
                stream = self.finishes[event]
                if event in self.queued[stream]:
                    self.queued[stream].remove(event)
                    self.last[stream] = event
        elif kind == "edge" and "activation" in record:
            activation = record["activation"]
            if activation not in self.activated or record["relation"] != "signal" or record["delay_ns"] != 0:
                raise ValueError("dynamic stream edge lacks its activating boundary or pure wait semantics")
            self.actual.add((activation, record["source"], record["target"]))

    def finish(self):
        missing = self.declared - self.activated
        if missing:
            self.findings.append(("stream_evidence", {}, {
                "missing_activations": len(missing), "events": sorted(missing)[:3],
                "reason": "completion alone cannot establish command acceptance or the wait's snapshot",
            }))
        for expected, actual, reason in ((self.expected, self.actual, "missing"), (self.actual, self.expected, "extra")):
            for activation, source, target in sorted(expected - actual):
                self.findings.append(("stream_frontier", {"event": activation}, {
                    "source": source, "target": target, "reason": reason + " dependency for the actual wait snapshot",
                }))
        return {
            **self.counts, "activation_points": len(self.declared), "activations_observed": len(self.activated),
            "activations_missing": len(missing), "required_dependencies": len(self.expected),
            "matched_dependencies": len(self.expected & self.actual), "extra_dependencies": len(self.actual - self.expected),
            "scope": "Run-local command acceptance and host stream-wait snapshots, including empty queues, completed frontiers and pending commands. Queue facts are derived from graph submission and actual activation/completion records, not dynamic edge answers. Excludes native API interception completeness and cross-run timing accuracy.",
        }
