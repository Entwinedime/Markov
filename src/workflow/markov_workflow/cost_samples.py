"""Publish source service samples keyed by normalized template work and bound geometry."""

from bisect import bisect_left
from collections import Counter, defaultdict
from dataclasses import dataclass
import json

from markov_adapters.artifacts import write_json


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
        self.rows = {}

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

    def document(self, unmeasured):
        return {
            "domain": self.catalog.domain, "definitions": self.catalog.definitions,
            "samples": list(self.rows.values()), "unmeasured": dict(unmeasured),
        }


@dataclass(frozen=True, slots=True)
class CollectiveInterval:
    position: tuple[int, int, int]
    payload: list[int] | None
    start_ns: int | None
    finish_ns: int | None
    radius_ns: int


class CollectiveServices:
    """Match measured peers within one native production and use the last participant's device-local interval."""

    def __init__(self, facts, rank):
        calls = defaultdict(lambda: defaultdict(list))
        for actor, document in facts.items():
            measurement = document["measurements"]
            points = {(row["resource"], row["order"]): row["time_ns"] for row in measurement["coordinates"]}
            radius = measurement["device_clock_radius_ns"]
            for scope, resource, begin, end, payload in measurement["collectives"]:
                key = self.identity(document["scopes"], scope)
                if key is None:
                    continue
                position = resource, begin, end
                start, finish = points[resource, begin], points[resource, end]
                calls[key][actor].append(CollectiveInterval(position, payload, start, finish, radius))

        self.samples = {}
        for key, actors in calls.items():
            members = key[1]
            if rank not in actors or actors.keys() != set(members) or any(len(rows) != 1 for rows in actors.values()):
                continue
            own, = actors[rank]
            peers = [rows[0] for rows in actors.values()]
            if own.payload is None or any(peer.payload != own.payload or peer.start_ns is None or peer.finish_ns is None
                                          for peer in peers):
                continue

            # Calibration is used to establish order only. The duration stays on a single device clock.
            last = max(peers, key=lambda row: row.start_ns)
            if any(last.start_ns - last.radius_ns <= peer.start_ns + peer.radius_ns for peer in peers if peer is not last):
                continue
            self.samples[own.position] = last.finish_ns - last.start_ns, own.payload

    @staticmethod
    def identity(scopes, identity):
        exchange = scopes[identity]["communication"]
        while identity is not None:
            scope = scopes[identity]
            work = scope["work"]
            if "phase" in work and "members" in work:
                batch = tuple((row["id"], row["context_tokens"], row["new_tokens"]) for row in work["members"])
                return exchange["channel"], tuple(exchange["members"]), exchange["operation"], exchange["order"], work["phase"], batch
            identity = scope["parent"]
        return None


class CostSamples:
    def __init__(self, arguments):
        self.output = arguments.output
        root = arguments.production
        rank = arguments.rank
        ranks = json.loads((root / "result.json").read_text())["observed_roles"]
        if rank not in ranks:
            raise ValueError("cost samples require an observed production rank")
        work = root / "work"
        self.catalog = WorkCatalog(work / f"{rank}-graph.json", arguments.work_identities)
        self.graph = self.catalog.graph
        facts = json.loads((work / f"{rank}-facts.json").read_text())
        measurement = facts["measurements"]
        if (self.graph["environment"] != facts["environment"]
                or self.catalog.window != measurement["window_ns"]):
            raise ValueError("cost measurements and coarse graph come from different native observations")

        self.collectives = {}
        if measurement["collectives"]:
            peers = {actor: facts if actor == rank else json.loads((work / f"{actor}-facts.json").read_text())
                     for actor in ranks}
            self.collectives = CollectiveServices(peers, rank).samples

        self.services = defaultdict(dict)
        for lane, begin, end, duration_ns in measurement["device_services"]:
            if duration_ns < 0 or (begin, end) in self.services[lane]:
                raise ValueError("physical service has a negative duration or repeated position")
            self.services[lane][begin, end] = duration_ns
        self.copies = {}
        for lane, begin, end, duration_ns, size, direction in measurement["synchronous_copies"]:
            if duration_ns < 0 or size <= 0 or (lane, begin, end) in self.copies:
                raise ValueError("synchronous copy has invalid or repeated service evidence")
            self.copies[lane, begin, end] = duration_ns, size, direction
        self.payload = [resource["payload"] for resource in facts["resources"]]
        self.positions = [[begin for begin, _ in ranges] for ranges in self.payload]

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
            copy = self.copies.get((lane, begin, end))
            return (copy[0], [copy[1], copy[2]], None) if copy is not None else (None, None, None)

        positions, payload = self.positions[lane], self.payload[lane]
        first = bisect_left(positions, begin)
        last = bisect_left(positions, end, lo=first)
        if first == last or (first and payload[first - 1][1] > begin) or payload[last - 1][1] > end:
            raise ValueError("device work does not enclose complete native payload")
        return sum(self.services[lane][start, finish] for start, finish in payload[first:last]), None, None

    def run(self):
        samples, absent = ServiceSamples(self.catalog), Counter()
        for piece, definition, interval, features in self.catalog:
            duration_ns, copy, collective = self.service(piece)
            if duration_ns is None:
                absent[piece["kind"]] += 1
                continue

            # Source request and physical resource IDs locate the observation only.
            samples.add(definition, interval, features, duration_ns, copy, collective)
        write_json(self.output, samples.document(absent), replace=False)
