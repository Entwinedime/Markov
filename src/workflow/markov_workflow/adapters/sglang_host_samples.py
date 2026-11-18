"""Match independently timed native CPU fragments to complete coarse work intervals."""

from collections import Counter, defaultdict
import json

from markov_adapters.artifacts import write_json
from markov_workflow.capture import CaptureProcess
from markov_workflow.cost_samples import ServiceSamples, WorkCatalog


class HostCostSamples:
    def __init__(self, arguments):
        self.arguments = arguments
        self.children = defaultdict(list)
        self.qualification = Counter()

    @staticmethod
    def read(path):
        return json.loads(path.read_text())

    def target(self, scope):
        name = scope["implementation"]
        return name if name in self.targets else name + ".forward"

    def capture(self, scopes, probe):
        source, observed = defaultdict(list), defaultdict(list)
        enclosing, batches = {}, []
        for scope in scopes:
            parent = scope["parent"]
            self.children[parent].append(scope)
            if scope["role"] == "run_batch":
                batch = len(batches)
                work = scope["work"]
                batches.append({"phase": work["phase"], "members": [member["id"] for member in work["members"]]})
            else:
                batch = enclosing.get(parent)
            enclosing[scope["id"]] = batch

            # Within-batch calls use their owning batch. Scheduler calls use the interval between batches.
            target = self.target(scope)
            if target in self.targets and scope["role"] != "segment":
                key = (batch is not None, batch if batch is not None else len(batches), target)
                source[key].append(scope)

        if batches != probe["batches"]:
            raise ValueError("native host observation changed the workload's actual batches")
        next_batch = 0
        for row in probe["intervals"]:
            batch = row["batch"]
            key = (batch is not None, batch if batch is not None else next_batch, row["target"])
            observed[key].append(row)
            if row["target"] == "sglang.srt.managers.scheduler.Scheduler.run_batch":
                next_batch += 1
        return source, observed

    def fragments(self, scope, observed):
        direct = self.children[scope["id"]]
        if not direct and not observed:
            return [scope]
        if len(direct) != 2 * len(observed) + 1 or [self.target(child) for child in direct[1::2]] != observed:
            return ()

        # The retained native cut is valid only when the sampled children partition the same caller.
        fragments = direct[::2]
        entry = "call.enter"
        for fragment in fragments:
            if fragment["role"] != "segment" or self.target(fragment) != self.target(scope):
                return ()
            continuation = fragment["work"]["continuation"]
            if continuation["entry"] != entry:
                return ()
            entry = continuation["exit"]
        return fragments if entry == "call.return" else ()

    @staticmethod
    def whole_work(catalog, facts):
        graph = catalog.graph
        events = graph["evidence"]["events"]
        work = defaultdict(list)
        for (piece, definition, interval, features), span_index in zip(catalog, graph["evidence"]["work_spans"], strict=True):
            if piece["kind"] != "host" or graph["resources"][piece["resource"]]["role"] != "control":
                continue
            scope = graph["scopes"][piece["scope"]]
            source = scope["bindings"]["source_scopes"]
            span = facts["spans"][span_index]
            start, finish = piece["start"], piece["finish"]
            if len(source) == 1 and (events[start]["order"], events[finish]["order"]) == (span["begin"], span["end"]):
                work[source[0]].append((definition, interval, features))
        return {source: pieces[0] for source, pieces in work.items() if len(pieces) == 1}

    def run(self):
        detailed = self.arguments.detailed
        lightweight = self.arguments.host
        source, host = (self.read(task / "execution.json")["inputs"] for task in (detailed, lightweight))
        for field in ("job", "deployment", "framework_revision"):
            if source[field] != host[field]:
                raise ValueError(f"host cost capture changes the measured native {field}")
        source_root = CaptureProcess.published(detailed, source["job"])
        host_root = CaptureProcess.published(lightweight, host["job"])
        instance, = self.read(lightweight / "result.json")["instances"]
        rank, = instance["native_participants"]
        probe = self.read(host_root / instance["host_samples"])
        catalog = WorkCatalog(self.arguments.graph, self.arguments.work_identities)
        graph = catalog.graph
        facts = self.read(self.arguments.facts)
        original = self.read(source_root / f"rank-{rank}.json")
        if (facts["measurements"]["window_ns"] != catalog.window
                or facts["environment"] != graph["environment"] or original["environment"] != graph["environment"]):
            raise ValueError("host measurement uses a graph from a different native capture")
        if probe["clock"] != "thread_cpu_ns" or probe["targets"] != host["host_targets"]:
            raise ValueError("host samples do not match their declared measurement")

        self.targets = set(probe["targets"])
        calls, observed = self.capture(original["scopes"], probe)
        work = self.whole_work(catalog, facts)
        samples = ServiceSamples(catalog)
        matched = 0
        for key, scopes in calls.items():
            records = observed[key]
            if (len(scopes) != len(records) or len({scope["thread"] for scope in scopes}) != 1
                    or len({row["thread"] for row in records}) != 1):
                self.qualification["native_call_census"] += len(scopes)
                continue
            for scope, row in zip(scopes, records, strict=True):
                fragments = self.fragments(scope, row["children"])
                if not fragments:
                    self.qualification["different_call_boundaries"] += 1
                    continue
                for fragment, raw_ns in zip(fragments, row["fragments_ns"], strict=True):
                    piece = work.get(fragment["id"])
                    duration = raw_ns - probe["timer_bracket_ns"]
                    if piece is None or duration <= 0:
                        self.qualification["partial_work" if piece is None else "below_clock_bracket"] += 1
                        continue

                    definition, interval, features = piece
                    read = fragment["bindings"].get("condition", {}).get("read", {})
                    if fragment["role"] == "query" and read.get("kind") in {"capacity", "clock"}:
                        features = {"phase": "", "axes": {}}
                    samples.add(definition, interval, features, duration)
                    matched += 1

        report = samples.document({"host": sum(piece["kind"] == "host" for piece in graph["work"]) - matched})
        report["qualification"] = {
            "timer_bracket_ns": probe["timer_bracket_ns"], "native_batches": len(probe["batches"]),
            "matched_works": matched, "misses": dict(self.qualification),
        }
        write_json(self.arguments.output, report, replace=False)
