"""Match independently timed native CPU fragments to complete coarse work intervals."""

from collections import Counter, defaultdict
import json

from markov_adapters.artifacts import write_json
from markov_collection.adapters.runtimes.ascend.host_alignment import AscendHostAlignment
from markov_collection.adapters.runtimes.ascend.trace import AscendTrace
from markov_workflow.capture import CaptureProcess
from markov_workflow.cost_samples import ServiceSamples, WorkCatalog


class HostCostSamples:
    def __init__(self, arguments):
        self.arguments = arguments
        self.qualification = Counter()

    @staticmethod
    def read(path):
        return json.loads(path.read_text())

    @staticmethod
    def records(path):
        with path.open() as source:
            return [json.loads(line) for line in source]

    @staticmethod
    def identity(scope):
        role, work = scope["role"], scope["work"]
        detail = None
        if role == "run_batch":
            detail = [work["phase"], [{key: value for key, value in member.items() if key != "slot"} for member in work["members"]]]
        elif role == "segment":
            detail = [work["continuation"], scope["bindings"]["condition"]]
        elif role in {"invocation", "query", "host_read", "batch_transform", "cache_match"}:
            detail = work.get("context")
        return role, scope["implementation"], scope["site"], json.dumps(detail, sort_keys=True)

    def children(self, scopes):
        indexed = defaultdict(list)
        for scope in scopes[1:]:
            indexed[scope["parent"]].append((self.identity(scope), scope["id"]))
        return indexed

    @staticmethod
    def group(scopes):
        grouped = defaultdict(list)
        for key, scope in scopes:
            grouped[key].append(scope)
        return grouped

    def match(self, scopes, observed):
        batches = [[self.identity(scope) for scope in rows if scope["role"] == "run_batch"] for rows in (scopes, observed)]
        if batches[0] != batches[1]:
            raise ValueError("native host observation changed the workload's actual batches")
        source, measured = (self.children(rows) for rows in (scopes, observed))
        pairs = {}
        pending = [(0, 0)]
        while pending:
            left, right = pending.pop()
            old, new = source[left], measured[right]
            if [key for key, _ in old] == [key for key, _ in new]:

                # A caller's charged interval requires the same ordered partition by its children.
                if left:
                    pairs[left] = right
                pending.extend((a, b) for (_, a), (_, b) in zip(old, new, strict=True))
                continue
            self.qualification["different_call_boundaries"] += bool(left)
            candidates = self.group(new)
            for key, values in self.group(old).items():
                matches = candidates.get(key, [])
                if len(values) == len(matches):
                    pending.extend(zip(values, matches, strict=True))
                else:
                    self.qualification["native_call_census"] += len(values)
        return pairs, len(batches[0])

    def run(self):
        detailed = self.arguments.detailed
        lightweight = self.arguments.host
        source, host = (self.read(task / "execution.json")["inputs"] for task in (detailed, lightweight))
        for field in ("deployment", "framework_revision"):
            if source[field] != host[field]:
                raise ValueError(f"host cost capture changes the measured native {field}")
        if (source["job"] | {"warmup": None}) != (host["job"] | {"warmup": None}):
            raise ValueError("host cost capture changes the measured native workload or configuration")
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
        if probe["clock"] != "monotonic_raw_ns" or not host["host_timing"]:
            raise ValueError("host samples do not match their declared measurement")

        pairs, batches = self.match(original["scopes"], probe["scopes"])
        timing = probe["timing"]
        directory = (host_root / instance["host_samples"]).parent
        queues = self.read(directory / timing["queues"])
        if queues["clock"] != "monotonic_raw_ns" or queues["unpaired"]:
            raise ValueError("native CPU timing requires paired queue evidence on the RAW clock")
        if timing["specialization_loads"]:
            raise ValueError(f"CPU measurement loaded {len(timing['specialization_loads'])} new specializations after warmup")
        profile, = (source_root / "native" / f"rank-{rank}").glob("*_ascend_pt")
        trace = AscendTrace(profile, source_root / "runtime", original, self.arguments.trace)
        alignment = AscendHostAlignment(
            trace, probe["scopes"], pairs, queues["commands"], self.records(directory / timing["runtime"]),
            self.records(directory / timing["activity"]), probe["auxiliary_intervals"], original["state_queries"],
        )

        evidence = graph["evidence"]["events"]
        samples = ServiceSamples(catalog)
        measured, missing = Counter(), Counter()
        for piece, definition, interval, features in catalog:
            if piece["kind"] != "host":
                continue
            resource = graph["resources"][piece["resource"]]
            scope = graph["scopes"][piece["scope"]]
            key = f"{resource['role']}/{scope['role']}"
            begin, end = (evidence[piece[side]]["order"] for side in ("start", "finish"))
            thread = int(resource["identity"].split("/")[1])
            duration = alignment.duration(thread, begin, end, resource["role"] == "submission")
            if duration is None:
                missing[key] += 1
                continue
            samples.add(definition, interval, features, duration)
            measured[key] += 1

        report = samples.document({"host": sum(missing.values())})
        report["qualification"] = {
            "native_batches": batches,
            "queue_correction_ns": queues["raw_correction_ns"],
            "measured": dict(measured), "missing": dict(missing),
            "alignment": dict(self.qualification + alignment.misses), "lost_threads": alignment.service.loss,
        }
        write_json(self.arguments.output, report, replace=False)
