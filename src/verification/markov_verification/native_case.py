"""Assemble one observed case independently of prediction and cross-run pairing."""

import json
from collections import Counter, defaultdict
from functools import cached_property

from markov_adapters.sglang.configuration import ServingInput

from .actions import ActionIdentity
from .bindings import BindingComparison
from .connections import Connections
from .contents import GraphContents
from .cpu_protocol import CpuProtocolEvidence
from .dependencies import CausalGraph
from .pd_payload import PdPayloadEvidence
from .protocol_reads import PdStateEvidence
from .resource_effects import ResourceEffectComparison
from .source_clock import ClockConstraints
from .source_effects import NativeEffectEvidence


class NativeCase:
    evidence_categories = frozenset({"native_evidence", "binding_evidence", "cpu_protocol_evidence", "missing_native_identity"})

    @classmethod
    def read(cls, capture, graphs):
        manifest = json.loads((capture / "capture.json").read_text())
        case = cls(capture, manifest, graphs)
        for instance in manifest["instances"]:
            for rank in instance["observed_roles"]:
                graph = json.loads((graphs / f"{rank}-graph.json").read_text())
                raw = json.loads((capture / f"rank-{rank}.json").read_text())
                case.observe(rank, graph, raw)
        case.finish()
        return case

    def __init__(self, capture, manifest, graphs):
        self.capture, self.manifest, self.graphs = capture, manifest, graphs
        self.groups = {rank: f"{instance['id']}/{rank}" for instance in manifest["instances"] for rank in instance["native_participants"]}
        self.ranks = {rank: instance["id"] for instance in manifest["instances"] for rank in instance["native_participants"]}
        self.observed_roles = {rank for instance in manifest["instances"] for rank in instance["observed_roles"]}
        if (len(self.groups) != sum(len(instance["native_participants"]) for instance in manifest["instances"])
                or len(self.observed_roles) != sum(len(instance["observed_roles"]) for instance in manifest["instances"])
                or self.observed_roles - self.groups.keys()):
            raise ValueError("native case has repeated participants or invalid observed workers")

        self.sources, self.dependencies, self.clocks = {}, {}, {}
        self.logical = {}
        self.measurements = {}
        self.connections = Connections()
        self.cpu = CpuProtocolEvidence(capture, manifest["instances"])
        self.protocol = PdStateEvidence()
        self.control_declarations = defaultdict(set)
        self.findings = []
        self.observed_actions = 0

    @cached_property
    def sessions(self):
        return [json.loads((self.capture / "instances" / str(index) / "session.json").read_text())
                for index, _ in enumerate(self.manifest["instances"])]

    def input_difference(self, source):
        target = ServingInput(source)
        for _, raw, _ in self.sources.values():
            native = raw["environment"]["implementation"]
            if target.model != {"path": native["model"]["_name_or_path"], "dtype": native["dtype"].removeprefix("torch.")}:
                return "model"
        instances = self.manifest["instances"]
        if len(instances) != len(target.instances):
            return "instances"
        for observed, session, instance in zip(instances, self.sessions, target.instances, strict=True):
            captured = dict(session["serving_input"])
            configuration = captured.pop("configuration")
            if observed["id"] != instance.identity or observed["mode"] != instance.mode or configuration != instance.configuration.values:
                return f"instance:{instance.identity}"
            if captured != target.workload:
                return "workload"
        return None

    def observe(self, rank, graph, raw, logical_coordinates=None):
        if rank not in self.observed_roles or rank in self.sources:
            raise ValueError("native worker observation is unexpected or repeated")
        if raw["environment"]["implementation"]["framework_revision"] != self.manifest["framework_revision"]:
            raise ValueError("native worker and capture disagree on their upstream framework revision")
        source = GraphContents(graph)
        source.validate_source(raw, rank)
        self.logical[rank] = set()
        if any(point["time_ns"] is None for point in graph["evidence"]["events"]):
            if logical_coordinates is None:
                facts = json.loads((self.graphs / f"{rank}-facts.json").read_text())
                runtime = NativeEffectEvidence(self.capture, rank, raw, facts, self.graphs / f"{rank}-sdk/trace.json")
                runtime.inspect(graph)
                logical_coordinates = runtime.logical_coordinates

                # SDK dictionaries are no longer needed while projecting the retained causal graph.
                del runtime, facts
            logical = {}
            for event, point in enumerate(graph["evidence"]["events"]):
                coordinate = graph["events"][event]["resource"], point["order"]
                if point["time_ns"] is None and coordinate in logical_coordinates:
                    logical[event] = logical_coordinates[coordinate]
            self.logical[rank] = set(logical)

            # A proven wait return can hand off a value produced by earlier work. The work itself still needs both times.
            measured = {event for work in graph["work"] for event in (work["start"], work["finish"])}
            for scope in graph["scopes"]:
                for port in scope["ports"]:
                    if (port["use"] == "data" and (port["direction"] == "input" or logical.get(port["event"]) is None)
                            or port["use"] == "signal" and port["direction"] == "output"):
                        measured.add(port["event"])
                    if "rendezvous" in port:
                        measured.add(port["rendezvous"])
                for access in scope["kv_accesses"] + scope["slot_accesses"]:
                    measured.update((access["start"], access["finish"]))
            if invalid := measured & self.logical[rank]:
                raise ValueError(f"elided waits at rank {rank} own measured boundaries: {sorted(invalid)}")
        dependencies = CausalGraph.observed(graph)
        clock = ClockConstraints(graph, raw["device_clock"])
        anchors = [event for event, point in enumerate(clock.points) if point is not None]

        # Project across unknown nodes, retaining one shared calibration for all measured causal paths.
        for left, targets in dependencies.projected(anchors).next.items():
            for right in targets:
                clock.constrain("causal", None, anchors[left], anchors[right])
        clock.finish()
        if clock.conflict:
            raise ValueError(f"native rank {rank} measurements have no common causal calibration: {clock.explain(graph)}")
        self.clocks[rank] = clock
        identities = {}
        for index, scope in enumerate(graph["scopes"]):
            if not ActionIdentity.selected(index, scope):
                continue
            self.observed_actions += 1
            try:
                identity = source.record(index, self.groups[rank])
            except ValueError as error:
                identity = {"group": self.groups[rank], "scope": index, "action": scope["role"]}
                self.findings.append(("missing_native_identity", identity, {"reason": str(error)}))
            else:
                self.control_declarations[source.control_key(index, identity)].add(source.declared_control(index))
            identities[index] = identity

        self.cpu.native_graph(rank, graph)
        self.protocol.observe(raw, self.groups[rank])
        self.dependencies[rank] = dependencies
        self.sources[rank] = source, raw, identities
        return source

    def completion_boundaries(self, rank, scope):
        return [boundary for boundary in self.sources[rank][0].completion_boundaries(scope)
                if boundary["event"] not in self.logical[rank]]

    def measured_service_pairs(self, rank, graph, raw, *, resources=None):
        if resources is None:
            resources = json.loads((self.graphs / f"{rank}-facts.json").read_text())["resources"]
        payload = [{tuple(row) for row in lane["payload"]} for lane in resources]
        queries = {(raw["scopes"][read["scope"]]["thread"], read["begin_counter"], read["end_counter"])
                   for read in raw["state_queries"]}
        events = graph["evidence"]["events"]
        result = set()
        for piece in graph["work"]:
            lane = piece["resource"]
            resource = graph["resources"][lane]
            start, finish = piece["start"], piece["finish"]
            bounds = events[start]["order"], events[finish]["order"]

            # One actual task/copy, or an independently cut CPU read, has no inter-task gap to charge.
            if piece["kind"] in {"device", "transfer", "communication"} and bounds in payload[lane]:
                result.add((start, finish))
            elif piece["kind"] == "host" and resource["kind"] == "thread":
                thread = int(resource["identity"].split("/")[1])
                if (thread, *bounds) in queries:
                    result.add((start, finish))
        return result

    def finish(self):
        if self.sources.keys() != self.observed_roles:
            raise ValueError("native case omits observed workers")
        if not self.observed_actions:
            raise ValueError("native case contains no observed actions")
        self.controls = {key for key, declarations in self.control_declarations.items() if declarations == {True}}
        registrations = {}
        for path in sorted((self.capture / "kv-pools").glob("*.json")):
            registration = json.loads(path.read_text())
            session = registration["registration"]["session"]
            if session in registrations:
                raise ValueError("native session identifies more than one registered pool")
            registrations[session] = registration
        registered_ranks = {session: value["parallel"]["rank"] for session, value in registrations.items()}

        # Registration covers initialized peers too; missing graphs never remove real PD endpoints.
        self.protocol.register(registrations, self.groups)
        self.histories = {}
        for rank, group in self.groups.items():
            path = self.capture / "resources" / f"rank-{rank}.json"
            if path.is_file():
                self.histories[group] = json.loads(path.read_text())
        self.bindings = BindingComparison(self.histories, {session: self.groups[rank] for session, rank in registered_ranks.items()})
        self.effects = ResourceEffectComparison()
        payload = PdPayloadEvidence(registrations, self.protocol.sessions, self.bindings.pools)
        for rank, (source, raw, identities) in self.sources.items():
            group = self.groups[rank]
            source.observed_pools(raw, registered_ranks, self.groups)
            payload.inspect(source, raw, group, self.dependencies[rank])
            self.connections.native(source, raw, rank, self.ranks[rank], self.dependencies[rank], identities)
            if group in self.histories:
                self.bindings.observed(source, raw, group, identities)
                self.effects.observed(self.histories[group], source.graph, group, raw)
            else:
                self.bindings.findings.append(("binding_evidence", {"group": group}, {"reason": "native resource history is absent"}))
        self.findings.extend(payload.findings)
        self.payload_counts = dict(payload.counts)

        # These run-local rounds and links are also the source for later prediction comparison.
        self.connections.native_communication(self.dependencies)
        self.pd_links = self.protocol.causal_dependencies(self.sources, self.groups)
        self.links = self.pd_links | {(source, target, "communication") for source, target in self.connections.native_communication_edges}
        self.graph = self.cpu.native(self.dependencies, self.links)
        self.readiness = self.protocol.inspect()

    def report(self, details):
        findings = [*self.findings, *self.connections.findings, *self.bindings.findings, *self.cpu.findings]
        counts = Counter(category for category, _, _ in findings)
        missing_pd = {name: len(self.readiness[name]) for name in (
            "missing_publications", "missing_reservation_receipts", "missing_reservation_updates", "missing_completion_updates", "missing_success_reads",
            "unobserved_transfer_peers",
        )}
        metadata_unknown = self.connections.counts.get("native_metadata_unobserved", 0)
        batch_unknown = any(self.connections.batch_sync.counts.get("native_" + name, 0)
                            for name in ("unobserved", "unobserved_returns", "inactive_unverified"))
        complete = (not findings and not any(missing_pd.values()) and not metadata_unknown and not batch_unknown
                    and self.connections.counts["native_communication_protocol_complete"] and self.cpu.report()["complete"])
        selected, seen = [], set()
        for category, identity, detail in findings:
            if details or category not in seen:
                selected.append({"category": category, "kind": "evidence_gap" if category in self.evidence_categories else "difference",
                                 "work": identity, **detail})
            seen.add(category)
        return {
            "status": "differences" if counts.keys() - self.evidence_categories else "consistent" if complete else "incomplete",
            "workers": len(self.sources), "actions": self.observed_actions, "counts": dict(counts), "findings": selected,
            "model_communication": {name: self.connections.counts[name] for name in (
                "communication_channels", "communication_exchanges", "native_communication_protocol_complete", "native_peer_row_pairs",
            )},
            "model_links": len(self.connections.native_communication_edges), "pd_links": len(self.pd_links),
            "cpu_protocol": self.cpu.report(), "metadata_unobserved": metadata_unknown,
            "missing_pd_prerequisites": missing_pd,
            "physical_bindings": {"observed_request_bindings": len(self.bindings.native),
                                  "unconsumed_snapshots": self.bindings.unconsumed_snapshots},
            "all_rank_events": None if self.graph is None else len(self.graph.next),
            "logical_wait_boundaries": sum(map(len, self.logical.values())),
            "pd_payload": self.payload_counts,
            "resource_reads": sum(value["count"] for value in self.effects.reads["native"].values()),
            "scope": "Original worker declarations, empty logical resource pools, getter checkpoints, pool bindings, PD byte footprints/metadata and layer/K-V correspondence, enqueue slot ownership and retained page/row intervals, model rounds, CPU messages and PD prerequisites. No prediction, occurrence pairing, numerical payload verification or exact notification time.",
        }
