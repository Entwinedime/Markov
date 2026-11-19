"""Match independently timed native CPU fragments to complete coarse work intervals."""

from collections import Counter, defaultdict
from dataclasses import dataclass
import json

from markov_adapters.artifacts import write_json
from markov_adapters.pytorch import CpuProtocolWork
from markov_adapters.sglang.configuration import CaptureInput
from markov_collection.adapters.runtimes.ascend.host_alignment import AscendHostAlignment
from markov_collection.adapters.runtimes.ascend.trace import AscendTrace
from markov_workflow.capture import CaptureProcess
from markov_workflow.cost_samples import ServiceSamples, WorkCatalog


@dataclass(frozen=True, slots=True)
class HostRecipe:
    name: str
    job: dict
    requires_cold: bool = False


class HostCostSamples:
    batch_roles = {"run_batch", "launch", "output_prepare", "output_skip"}

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
    def recipes(job):
        capture = CaptureInput(job)
        yield HostRecipe("observed-submission", job)
        if capture.warmup != capture.workload:
            yield HostRecipe("primed-submission", {**job, "warmup": job["workload"]}, requires_cold=True)
        groups = [[request["id"]] for request in capture.workload["requests"]]
        if not all(len(group) == 1 for group in capture.submission_groups):
            yield HostRecipe("individual-requests", {**job, "submission_groups": groups, "warmup": job["workload"]})

    @staticmethod
    def supports(work, resource):
        return work["kind"] == "host" or (work["kind"] == "collective" and resource["kind"] == "thread")

    @classmethod
    def identity(cls, scope):
        role, work = scope["role"], scope["work"]
        detail = None
        if role in cls.batch_roles:
            detail = [work["phase"], [{key: value for key, value in member.items() if key != "slot"} for member in work["members"]]]
        elif role == "segment":
            detail = [work["continuation"], scope["bindings"]["condition"]]
        elif role in {"invocation", "query", "host_read", "batch_transform", "cache_match", "kv_reservation"}:
            detail = work.get("context")
        elif role == "metadata_collective":
            detail = [scope["parameters"], scope["bindings"]["condition"], work["context"].get("poll_requests")]
        elif role in {"transfer_send", "transfer_receive"}:
            detail = [work["context"]["message"], scope["bindings"]["dtype"]]
        elif role in {"transfer_send_wait", "transfer_receive_wait", "signal_wait"}:
            context = work["context"]
            producer = context["producer"]
            detail = [context["device_wait"], context.get("wait_stream"), producer.get("record_stream"), producer["message"]]
        elif role == "stream_wait":
            detail = work["context"]["streams"]
        elif role == "signal_record":
            detail = work["context"]["record_stream"]
        elif role == "gather":
            detail = scope["bindings"].get("collective")
        return role, scope["implementation"], scope["site"], json.dumps(detail, sort_keys=True)

    def index(self, scopes, paths, typed, excluded):
        routes, children = {0: 0}, defaultdict(list)
        identities = {0: None}
        for scope in scopes[1:]:
            identity, parent = scope["id"], scope["parent"]
            if "service" in scope or identity in excluded or parent not in routes:
                continue
            call = self.identity(scope)
            children[parent].append(call)
            identities[identity] = call

            # Batch geometry drives its model subtree; other enclosing queues only locate the local call.
            component = call if scope["role"] in self.batch_roles else call[:3]
            routes[identity] = paths.setdefault((routes[parent], component), len(paths) + 1)
        indexed = defaultdict(list)
        for identity, route in routes.items():
            if not identity:
                continue
            call = identities[identity]
            scope = scopes[identity]

            # Declared local operands are sufficient even when the containing scheduler queue changes.
            arguments = json.dumps(scope.get("parameters"), sort_keys=True) if call[:3] in typed else identities[scope["parent"]]
            indexed[route, call, arguments, tuple(children[identity])].append(identity)
        return indexed

    def match(self, scopes, observed, excluded):
        paths = {}
        typed = {(scope["role"], scope["implementation"], scope["site"]) for scope in observed if "parameters" in scope}
        source = self.index(scopes, paths, typed, ())
        measured = self.index(observed, paths, typed, excluded)
        pairs = {}
        for call, previous in source.items():
            timed = measured.get(call, ())

            # Polling changes the number of invocations; each equally partitioned observation supplies one sample.
            for original, occurrence in zip(previous, timed, strict=False):
                pairs[original] = occurrence
            self.qualification["unmatched_calls"] += max(0, len(previous) - len(timed))
        return pairs

    @staticmethod
    def cold_scopes(probe):
        excluded = set()
        for load in probe["timing"]["specialization_loads"]:
            time = load["raw_ns"]
            active = {scope["id"]: scope["parent"] for scope in probe["scopes"][1:]
                      if scope["raw_interval"][0] <= time < scope["raw_interval"][1]}
            if not active:
                raise ValueError("post-warmup specialization lies outside the observed native calls")

            # A parent interval may include the load even when the allocator runs before model execution.
            excluded.update(scope for scope, parent in active.items() if parent not in active)
        return excluded

    def collectives(self, records, rank):
        channels = defaultdict(lambda: defaultdict(list))
        for actor, record in records.items():
            for scope in record["scopes"]:
                group = scope.get("group")
                if group is None or scope["bindings"]["condition"]["collective"]["root"] is not None:
                    continue
                channels[group["channel"], tuple(group["members"])][actor].append(scope)

        durations = {}
        for (_, members), actors in channels.items():
            if rank not in actors:
                continue
            if set(actors) != set(members) or len({len(rows) for rows in actors.values()}) != 1:
                self.qualification["unpaired_collectives"] += len(actors[rank])
                continue
            local = members.index(rank)
            for participants in zip(*(actors[actor] for actor in members), strict=True):
                if len({self.identity(scope) for scope in participants}) != 1:
                    self.qualification["collective_operands"] += 1
                    continue

                # All ranks share the host RAW clock. The last entry marks the end of peer wait.
                arrival = max(scope["raw_interval"][0] for scope in participants)
                if any(scope["raw_interval"][1] < arrival for scope in participants):
                    self.qualification["collective_clock"] += 1
                    continue
                scope = participants[local]
                durations[scope["id"]] = scope["raw_interval"][1] - arrival
        return durations

    @classmethod
    def external_work(cls, capture):
        instances = cls.read(capture / "capture.json")["instances"]
        records = {rank: cls.read(capture / "protocol" / f"rank-{rank}.json")
                   for instance in instances for rank in instance["native_participants"]}
        demands = {rank: [(identity, "host", features) for identity, features, _ in CpuProtocolWork.local(record)]
                   for rank, record in records.items()}
        for identity, features in CpuProtocolWork(records).wires():
            demands[identity["source"]].append((identity, "transfer", features))
        return demands

    @staticmethod
    def cpu_services(samples, protocol, service):
        for identity, features, interval in CpuProtocolWork.local(protocol):
            duration = service.duration(interval["thread"], interval["begin_raw_ns"], interval["end_raw_ns"], False)
            if duration is not None:

                # The waiting thread can sleep while the transport progresses. Count its actual CPU service only.
                samples.add_external(identity, "host", features, duration)

    def run(self):
        detailed = self.arguments.detailed
        lightweight = self.arguments.host
        source, host = (self.read(task / "execution.json")["inputs"] for task in (detailed, lightweight))
        for field in ("deployment", "framework_revision"):
            if source[field] != host[field]:
                raise ValueError(f"host cost capture changes the measured native {field}")
        inputs = ("model", "instances", "workload", "observe_all_workers")
        if any(source["job"][field] != host["job"][field] for field in inputs):
            raise ValueError("host cost capture changes the measured native workload or configuration")
        source_root = CaptureProcess.published(detailed, source["job"])
        host_root = CaptureProcess.published(lightweight, host["job"])
        catalog = WorkCatalog(self.arguments.graph, self.arguments.work_identities)
        graph = catalog.graph
        rank = graph["environment"]["parallel"]["rank"]
        instance, = (candidate for candidate in self.read(lightweight / "result.json")["instances"]
                     if rank in candidate["native_participants"])
        paths = {int(actor): host_root / relative for actor, relative in instance["host_samples"].items()}
        path = paths[rank]
        records = {actor: self.read(measurement) for actor, measurement in paths.items()}
        probe = records[rank]
        facts = self.read(self.arguments.facts)
        original = self.read(source_root / f"rank-{rank}.json")
        if (facts["measurements"]["window_ns"] != catalog.window
                or facts["environment"] != graph["environment"] or original["environment"] != graph["environment"]):
            raise ValueError("host measurement uses a graph from a different native capture")
        if not host["host_timing"] or any(record["rank"] != actor or record["clock"] != "monotonic_raw_ns"
                                          for actor, record in records.items()):
            raise ValueError("host samples do not match their declared measurement")

        excluded = self.cold_scopes(probe)
        pairs = self.match(original["scopes"], probe["scopes"], excluded)
        collectives = self.collectives(records, rank)
        timing = probe["timing"]
        directory = path.parent
        queues = self.read(directory / timing["queues"])
        if queues["clock"] != "monotonic_raw_ns" or queues["unpaired"]:
            raise ValueError("native CPU timing requires paired queue evidence on the RAW clock")
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
            resource = graph["resources"][piece["resource"]]
            if not self.supports(piece, resource):
                continue
            scope = graph["scopes"][piece["scope"]]
            key = f"{resource['role']}/{scope['role']}"
            if piece["kind"] == "collective":
                source, = scope["bindings"]["source_scopes"]
                duration = collectives.get(pairs.get(source))
            else:
                begin, end = (evidence[piece[side]]["order"] for side in ("start", "finish"))
                thread = int(resource["identity"].split("/")[1])
                duration = alignment.duration(thread, begin, end, resource["role"] == "submission")
            if duration is None:
                missing[key] += 1
                continue
            samples.add(definition, interval, features, duration)
            measured[key] += 1

        if "protocol" in probe:
            self.cpu_services(samples, probe["protocol"], alignment.service)
            for row in self.read(host_root / instance["cpu_transports"]):
                if row["identity"]["source"] == rank:
                    for duration in row["observations_ns"]:
                        samples.add_external(row["identity"], "transfer", row["features"], duration)
        report = samples.document({"host": sum(missing.values())})
        report["observation"] = {"detailed": str(detailed.resolve()), "host": str(lightweight.resolve()), "rank": rank}
        batches = {"detailed": 0, "host": 0, "matched": 0}
        for scope in original["scopes"]:
            if scope["role"] == "run_batch":
                batches["detailed"] += 1
                batches["matched"] += scope["id"] in pairs
        batches["host"] = sum(scope["role"] == "run_batch" for scope in probe["scopes"])
        report["qualification"] = {
            "native_batches": batches,
            "excluded_cold_scopes": sorted(excluded),
            "queue_correction_ns": queues["raw_correction_ns"],
            "measured": dict(measured), "missing": dict(missing),
            "alignment": dict(self.qualification + alignment.misses), "lost_threads": alignment.service.loss,
        }
        write_json(self.arguments.output, report, replace=False)
