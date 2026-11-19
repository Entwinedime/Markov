"""Publish source service samples keyed by normalized template work and bound geometry."""

from bisect import bisect_left
from collections import Counter, defaultdict
from dataclasses import dataclass
import json
from pathlib import Path

from markov_adapters.artifacts import write_json
from markov_collection.adapters.runtimes.ascend.clock import DeviceCalibration


class WorkCatalog:
    def __init__(self, graph_path, identities_path):
        self.graph = json.loads(graph_path.read_text())
        identities = json.loads(identities_path.read_text())
        self.domain, self.definitions = identities["domain"], identities["definitions"]
        self.features = identities["features"]
        self.window = self.graph["evidence"]["window_ns"]
        if self.window != identities["window_ns"]:
            raise ValueError("work identities and coarse graph come from different native observations")
        self.positions = {
            (start, finish): (definition, interval) for start, finish, definition, interval in identities["intervals"]
        }
        expected = {(piece["start"], piece["finish"]) for piece in self.graph["work"]}
        if (len(expected) != len(self.graph["work"]) or expected != self.positions.keys()
                or len(self.positions) != len(identities["intervals"])):
            raise ValueError("work identities must cover each coarse graph interval once")

    def __iter__(self):
        for piece in self.graph["work"]:
            definition, interval = self.positions[piece["start"], piece["finish"]]
            if self.definitions[definition]["work"][interval]["kind"] != piece["kind"]:
                raise ValueError("normalized work identity changes the measured interval kind")
            yield piece, definition, interval, self.features[piece["scope"]]


class ServiceSamples:
    def __init__(self, catalog):
        self.catalog = catalog
        self.definitions = list(catalog.definitions)
        self.rows = {}
        self.external = {}

    def add(self, definition, interval, features, duration_ns, copy=None, collective=None):
        key = definition, interval, json.dumps([features, copy, collective], sort_keys=True)
        row = self.rows.setdefault(key, {
            "definition": definition, "interval": interval, "features": features, "observations_ns": [],
        })
        if copy is not None:
            row["copy"] = {"bytes": copy[0], "direction": copy[1]}
        if collective is not None:
            row["collective"] = {"dtype": collective[0], "algorithm": collective[1], "elements": collective[2]}
        row["observations_ns"].append(duration_ns)

    def add_external(self, identity, kind, features, duration_ns):
        key = json.dumps(identity, sort_keys=True)
        if key not in self.external:
            self.external[key] = len(self.definitions)
            self.definitions.append({"external": identity, "work": [{"kind": kind}]})
        self.add(self.external[key], 0, features, duration_ns)

    def document(self, unmeasured):
        return {
            "domain": self.catalog.domain, "definitions": self.definitions,
            "samples": list(self.rows.values()), "unmeasured": dict(unmeasured),
        }


@dataclass(frozen=True, slots=True)
class CollectiveInterval:
    position: tuple[int, int, int]
    payload: list[int]
    start_ns: int
    finish_ns: int
    start_bounds_ns: tuple[int, int]
    finish_bounds_ns: tuple[int, int]
    required: tuple[int, ...]
    peer_rows: dict | None


class CollectiveServices:
    """Qualify service from the actual participants and posts in one native production."""

    def __init__(self, facts, capture: Path, rank):
        channels = defaultdict(dict)
        measurements = {}
        for actor, document in facts.items():
            for channel, posts in self.posts(document):
                channels[channel][actor] = posts

            measurement = document["measurements"]
            points = {(row["resource"], row["order"]): row["time_ns"] for row in measurement["coordinates"]}
            scopes = document["scopes"]
            peer_rows = measurement.get("collective_peer_rows", {})
            native = json.loads((capture / f"rank-{actor}.json").read_text())
            if native["environment"] != document["environment"]:
                raise ValueError("collective calibration and worker facts come from different environments")
            clock = DeviceCalibration(native["device_clock"])
            intervals = {}
            for scope, resource, begin, end, payload in measurement["collectives"]:
                start, finish = points[resource, begin], points[resource, end]
                if payload is None or start is None or finish is None:
                    continue
                required = tuple(scopes[scope]["communication"]["required"])
                intervals[scope] = CollectiveInterval((resource, begin, end), payload, start, finish,
                                                       clock.bounds(start), clock.bounds(finish), required, peer_rows.get(str(scope)))
            measurements[actor] = intervals

        self.samples = {}
        for actors in channels.values():
            if rank not in actors:
                continue
            operations = [(operation, members) for _, operation, members in actors[rank]]
            members = operations[0][1]
            if actors.keys() != set(members) or any(
                [(operation, group) for _, operation, group in posts] != operations for posts in actors.values()
            ):
                continue

            # Align the complete native post sequence before discarding any unmeasured round.
            for ordinal, (operation, _) in enumerate(operations):
                peers = {actor: measurements[actor].get(actors[actor][ordinal][0]) for actor in members}
                if None in peers.values():
                    continue
                own = peers[rank]
                if own.peer_rows is not None:
                    duration = self.variable_service(peers, members, rank)
                else:
                    duration = self.uniform_service(peers.values(), operation)
                if duration is not None:
                    self.samples[own.position] = duration, own.payload

    @staticmethod
    def posts(document):
        channels = defaultdict(list)
        spans = document["spans"]
        for index, scope in enumerate(document["scopes"]):
            exchange = scope.get("communication")
            if exchange is None:
                continue
            entry = next((port["at"] for port in scope["ports"] if port["name"] == "call.enter"), None)
            lane, order = (None, None) if entry is None else (spans[entry["span"]]["resource"], entry["order"])
            channels[exchange["channel"]].append((lane, order, index, exchange["operation"], tuple(exchange["members"])))

        # A single observed issuing thread establishes the channel's order independently of batch annotations.
        for channel, posts in channels.items():
            lanes = {lane for lane, _, _, _, _ in posts}
            orders = {order for _, order, _, _, _ in posts}
            if len(lanes) != 1 or None in lanes or len(orders) != len(posts):
                continue
            lane, = lanes
            if document["resources"][lane]["kind"] != "thread":
                continue
            yield channel, [(index, operation, members) for _, _, index, operation, members in sorted(posts)]

    @staticmethod
    def uniform_service(peers, operation):
        payloads = {(row.payload[0], row.payload[2]) if operation == "transfer" else tuple(row.payload) for row in peers}
        if len(payloads) != 1:
            return None

        # Calibration establishes order only. The duration stays on the last participant's device clock.
        last = max(peers, key=lambda row: row.start_ns)
        if any(last.start_bounds_ns[0] <= peer.start_bounds_ns[1] for peer in peers if peer is not last):
            return None
        return last.finish_ns - last.start_ns

    @staticmethod
    def variable_service(peers, members, rank):
        own = peers[rank]
        local = members.index(rank)
        sent, received = (own.peer_rows[name] for name in ("send", "receive"))
        for index, actor in enumerate(members):
            peer = peers[actor]
            if (peer.peer_rows is None or peer.payload[:2] != own.payload[:2]
                    or sent[index] != peer.peer_rows["receive"][local] or received[index] != peer.peer_rows["send"][local]):
                return None

        # Completion waits for the last required peer, so take the minimum of the finish-minus-start ranges.
        bounds = []
        for actor in own.required:
            peer = peers[actor]
            if actor == rank:
                duration = own.finish_ns - peer.start_ns
                bounds.append((duration, duration))
            else:
                lower = own.finish_bounds_ns[0] - peer.start_bounds_ns[1]
                upper = own.finish_bounds_ns[1] - peer.start_bounds_ns[0]
                bounds.append((lower, upper))
        lower = min(value[0] for value in bounds)
        upper = min(value[1] for value in bounds)
        return (lower + upper) // 2 if lower > 0 else None


class ObservedServices:
    """Resolve measurements from the same observation by their actual resource intervals."""

    def __init__(self, graph, facts, collectives):
        self.graph, self.collectives = graph, collectives
        measurement = facts["measurements"]
        if (graph["environment"] != facts["environment"]
                or graph["evidence"]["window_ns"] != measurement["window_ns"]):
            raise ValueError("cost measurements and coarse graph come from different native observations")

        self.host_positions = {}
        for lane, order, service_ns in measurement.get("host_service_positions", ()):
            if service_ns < 0 or (lane, order) in self.host_positions:
                raise ValueError("CPU service has a negative prefix or repeated position")
            self.host_positions[lane, order] = service_ns
        self.services = {}
        for lane, begin, end, duration_ns in measurement.get("device_services", ()):
            if duration_ns < 0 or (lane, begin, end) in self.services:
                raise ValueError("physical service has a negative duration or repeated position")
            self.services[lane, begin, end] = duration_ns
        self.transfers = {}
        for lane, begin, end, duration_ns, size, direction in measurement.get("synchronous_transfers", ()):
            if duration_ns < 0 or size <= 0 or (lane, begin, end) in self.transfers:
                raise ValueError("synchronous transfer has invalid or repeated service evidence")
            self.transfers[lane, begin, end] = duration_ns, size, direction
        self.payload = [resource["payload"] for resource in facts["resources"]]
        self.positions = [[begin for begin, _ in ranges] for ranges in self.payload]

    def host_service(self, piece):
        lane = piece["resource"]
        evidence = self.graph["evidence"]["events"]
        begin, end = (self.host_positions.get((lane, evidence[piece[key]]["order"])) for key in ("start", "finish"))
        if begin is None or end is None:
            return None
        if end < begin:
            raise ValueError("CPU service prefixes reverse work order")
        return end - begin

    def service(self, piece):
        lane = piece["resource"]
        evidence = self.graph["evidence"]["events"]
        begin, end = (evidence[piece[side]]["order"] for side in ("start", "finish"))
        if piece["kind"] == "collective":
            duration, payload = self.collectives.get((lane, begin, end), (None, None))
            return duration, None, payload
        if piece["kind"] == "communication":
            return None, None, None
        if self.graph["resources"][lane]["kind"] != "stream":
            if piece["kind"] != "transfer":
                return None, None, None
            copy = self.transfers.get((lane, begin, end))
            return (copy[0], [copy[1], copy[2]], None) if copy is not None else (None, None, None)

        positions, payload = self.positions[lane], self.payload[lane]
        first = bisect_left(positions, begin)
        last = bisect_left(positions, end, lo=first)
        if first == last or (first and payload[first - 1][1] > begin) or payload[last - 1][1] > end:
            raise ValueError("device work does not enclose complete native payload")
        durations = [self.services.get((lane, start, finish)) for start, finish in payload[first:last]]
        return (None if None in durations else sum(durations)), None, None


class CostSamples:
    def __init__(self, arguments):
        self.output = arguments.output
        root, rank = arguments.production, arguments.rank
        self.observation = {"production": str(root.resolve()), "rank": rank}
        ranks = json.loads((root / "result.json").read_text())["observed_roles"]
        if rank not in ranks:
            raise ValueError("cost samples require an observed production rank")
        work = root / "work"
        self.catalog = WorkCatalog(work / f"{rank}-graph.json", arguments.work_identities)
        facts = json.loads((work / f"{rank}-facts.json").read_text())
        measurement = facts["measurements"]
        peers = {}
        if measurement["collectives"] or "pd_protocol" in measurement:
            peers = {actor: facts if actor == rank else json.loads((work / f"{actor}-facts.json").read_text())
                     for actor in ranks}
        collectives = {}
        if measurement["collectives"]:
            capture = Path(json.loads((root / "production.json").read_text())["inputs"]["capture_path"])
            collectives = CollectiveServices(peers, capture, rank).samples
        self.services = ObservedServices(self.catalog.graph, facts, collectives)
        self.notifications = []
        if "pd_protocol" in measurement:
            from markov_workflow.adapters.sglang_pd_samples import PdNotificationSamples

            protocol = {actor: body["measurements"]["pd_protocol"] for actor, body in peers.items()}
            self.notifications = list(PdNotificationSamples(protocol).intervals(rank))

    def run(self):
        samples, absent = ServiceSamples(self.catalog), Counter()
        for piece, definition, interval, features in self.catalog:
            duration_ns, copy, collective = self.services.service(piece)
            if duration_ns is None:
                absent[piece["kind"]] += 1
                continue

            # Source request and physical resource IDs locate the observation only.
            samples.add(definition, interval, features, duration_ns, copy, collective)
        for measured in self.notifications:
            span = measured["duration_bounds_ns"]
            if span is None:
                absent["notification"] += 1
                continue
            features = {"phase": "", "axes": {"parameter.dependencies": measured["dependencies"]}}
            samples.add_external({"operation": measured["operation"]}, "transfer", features, sum(span) // 2)

        report = samples.document(absent)
        report["observation"] = self.observation
        if self.notifications:
            report["notification_measurements"] = self.notifications
        write_json(self.output, report, replace=False)
