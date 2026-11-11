"""Join each run's actual CPU P2P facts without cross-run message matching."""

import json
from collections import Counter, defaultdict
from math import prod

from dependencies import CausalGraph


class CpuProtocolEvidence:
    def __init__(self, capture, instances):
        self.capture, self.instances = capture, instances
        self.counts, self.findings = Counter(), []
        self.native_threads = defaultdict(lambda: defaultdict(list))
        self.native_groups, self.predicted_groups, self.actors = {}, {}, {}
        self.messages = {origin: defaultdict(dict) for origin in ("native", "predicted")}
        self.points = {origin: set() for origin in self.messages}
        self.edges = {origin: set() for origin in self.messages}
        self.last_cpu, self.returns, self.horizons = {}, {}, {}
        self.terminals = []
        self.terminals_checked = False
        self.sequence = Counter()
        self.predicted_scope = False

    def missing(self, reason, **detail):
        self.counts[reason] += 1
        self.findings.append(("cpu_protocol_evidence", {}, {"reason": reason, **detail}))

    def point(self, origin, rank):
        self.sequence[origin, rank] -= 1
        point = rank, self.sequence[origin, rank]
        self.points[origin].add(point)
        return point

    def native_graph(self, rank, graph):
        for event, (node, evidence) in enumerate(zip(graph["events"], graph["evidence"]["events"], strict=True)):
            resource = graph["resources"][node["resource"]]
            if resource["kind"] == "thread":
                thread = int(resource["identity"].removeprefix("thread/"))
                positions = self.native_threads[rank, thread]
                positions[evidence["order"]].append((rank, event))

    def native_interval(self, rank, interval):
        begin, end = interval["begin_counter"], interval["end_counter"]
        if type(begin) is not int or type(end) is not int or begin > end:
            raise ValueError("CPU protocol has an invalid native counter interval")
        positions = self.native_threads[rank, interval["thread"]]
        endpoints = []
        for counter in (begin, end):
            if any(event >= 0 for _, event in positions[counter]):
                self.missing("native_protocol_counter_ties_coarse_boundary", rank=rank, thread=interval["thread"], counter=counter)
            point = self.point("native", rank)
            positions[counter].append(point)
            endpoints.append(point)
        self.edges["native"].add(tuple(endpoints))
        return endpoints

    def post(self, origin, rank, key, descriptor, entered):
        group, source, target, tag, position = key
        route = group, source, target, tag
        if (route != tuple(descriptor[name] for name in ("group", "source", "target", "tag"))
                or source == target or rank != (target if descriptor["receiving"] else source)
                or type(descriptor["receiving"]) is not bool
                or type(position) is not int or position < 0
                or any(type(size) is not int or size < 0 for size in descriptor["shape"])):
            raise ValueError("CPU protocol post has inconsistent route, actor or tensor geometry")
        peers = self.messages[origin][key]
        if rank in peers:
            raise ValueError("CPU protocol repeats a local post identity")
        peers[rank] = {"entered": entered, "consumed": None, "dtype": descriptor["dtype"], "elements": prod(descriptor["shape"])}
        self.counts[origin + "_posts"] += 1
        return peers[rank]

    def load_native(self):
        participants = []
        for instance in self.instances:
            if "native_participants" not in instance:
                self.missing("missing_native_participant_manifest", instance=instance["id"])
                continue
            ranks = instance["native_participants"]
            if not ranks or len(set(ranks)) != len(ranks) or not set(instance["observed_roles"]) <= set(ranks):
                raise ValueError("CPU protocol requires the complete native participant manifest")
            for rank in ranks:
                if rank in self.native_groups:
                    raise ValueError("CPU protocol repeats a native participant")
                self.native_groups[rank] = instance["id"]
                participants.append(rank)
        for rank in participants:
            path = self.capture / "protocol" / f"rank-{rank}.json"
            if not path.is_file():
                self.missing("missing_native_protocol", rank=rank)
                continue
            document = json.loads(path.read_text())
            if document["backend"] != "gloo" or set(document["operations"]) != {"send", "recv", "isend", "irecv"}:
                raise ValueError("CPU protocol observation declares a different operation scope")
            start = {tuple(row["route"]): row["position"] for row in document["start_positions"]}
            end = {tuple(row["route"]): row["position"] for row in document["end_positions"]}
            observed = Counter()
            posting_threads = defaultdict(set)
            for route, floor in start.items():
                if not 0 <= floor <= end[route]:
                    raise ValueError("CPU protocol has a reversed observation horizon")
            self.horizons[rank] = {route: (start.get(route, 0), limit) for route, limit in end.items()}
            for post in document["posts"]:
                key = tuple(post["key"])
                route, position = key[:-1], key[-1]
                posting_threads[route].add(post["interval"]["thread"])
                if not position < end[route]:
                    raise ValueError("CPU protocol post lies outside its declared channel history")
                source, target = key[1:3]
                if self.native_groups[source] != self.native_groups[target] or self.native_groups[rank] != self.native_groups[source]:
                    raise ValueError("CPU protocol crosses independent native worlds")
                entered, returned = self.native_interval(rank, post["interval"])
                endpoint = self.post("native", rank, key, post["descriptor"], entered)
                if post["consumed"] is not None:
                    _, consumed = self.native_interval(rank, post["consumed"])
                    if post["consumed"]["end_counter"] < post["interval"]["end_counter"]:
                        raise ValueError("CPU Work consumption precedes its post return")
                    endpoint["consumed"] = consumed
                    self.edges["native"].add((returned, consumed))
                observed[route] += position >= start.get(route, 0)
            for route, limit in end.items():
                if observed[route] != limit - start.get(route, 0):
                    self.missing("native_posts_missing_from_horizon", rank=rank, route=route)
                if len(posting_threads[route]) > 1:
                    self.missing("native_channel_has_multiple_posting_threads", rank=rank, route=route)

    def observe(self, row):
        kind = row["kind"]
        if kind == "resources":
            self.predicted_groups = {group["id"]: (group["rank"], instance["id"])
                                     for instance in row["instances"] for group in instance["groups"]}
        elif kind == "graph":
            graph, events = row["graph"], row["event_ids"]
            ports = {port["name"]: events[port["event"]] for port in graph["scopes"][0]["ports"]
                     if port["name"] in {"call.enter", "call.return"}}
            entered = next(port for port in graph["scopes"][0]["ports"] if port["name"] == "call.enter")
            rank = graph["resources"][graph["events"][entered["event"]]["resource"]]["rank"]
            if rank in self.last_cpu:
                self.edges["predicted"].add((self.last_cpu[rank], (rank, ports["call.enter"])))
            self.returns[ports["call.return"]] = rank
        elif kind == "action_values" and "outputs" in row["values"]:
            rank, _ = self.predicted_groups[row["group"]]
            if self.returns.pop(row["boundary"]) != rank:
                raise ValueError("CPU protocol action return belongs to another participant")
            self.last_cpu[rank] = rank, row["boundary"]
        elif kind == "native_protocol":
            record, group = row["record"], row["group"]
            if record["phase"] == "scope":
                if self.predicted_scope or record["backend"] != "gloo" or set(record["operations"]) != {"send", "recv", "isend", "irecv"}:
                    raise ValueError("CPU broker observation has a competing or different protocol scope")
                self.actors = {actor["rank"]: actor["group"] for actor in record["participants"]}
                if (len(self.actors) != len(record["participants"])
                        or set(self.actors.values()) != set(self.predicted_groups)
                        or len(self.actors) != len(self.predicted_groups)):
                    raise ValueError("CPU broker observation omits or repeats a native participant")
                self.predicted_scope = True
                return
            rank, _ = self.predicted_groups[group]
            actor = record["rank"]
            if not self.predicted_scope or self.actors[actor] != group:
                raise ValueError("CPU broker actor changes its native participant")
            point = self.point("predicted", rank)
            if rank in self.last_cpu:
                self.edges["predicted"].add((self.last_cpu[rank], point))
            self.last_cpu[rank] = point
            key = tuple(record["key"])
            if record["phase"] == "post":
                if self.predicted_groups[self.actors[key[1]]][1] != self.predicted_groups[self.actors[key[2]]][1]:
                    raise ValueError("CPU broker P2P crosses independent native worlds")
                self.post("predicted", actor, key, record["descriptor"], point)
            elif record["phase"] == "consumed":
                peers = self.messages["predicted"][key]
                if len(peers) != 2:
                    raise ValueError("CPU broker consumption precedes the matching peer post")
                endpoint = peers[actor]
                if endpoint["consumed"] is not None:
                    raise ValueError("CPU Work consumption is recorded twice")
                endpoint["consumed"] = point
            else:
                raise ValueError("CPU protocol has an unknown observation phase")

    def connect_messages(self, origin):
        for key, peers in self.messages[origin].items():
            self.counts[origin + "_consumptions"] += sum(peer["consumed"] is not None for peer in peers.values())
            self.counts[origin + "_unconsumed_posts"] += sum(peer["consumed"] is None for peer in peers.values())
            if len({(peer["dtype"], peer["elements"]) for peer in peers.values()}) != 1:
                raise ValueError("CPU protocol endpoints disagree on wire dtype or element count")
            if len(peers) == 2:
                self.counts[origin + "_messages"] += 1
                for destination in peers.values():
                    if destination["consumed"] is not None:

                        # Both posts precede either completion; the peer need not have consumed its Work.
                        for source in peers.values():
                            if source["entered"] != destination["consumed"]:
                                self.edges[origin].add((source["entered"], destination["consumed"]))
                continue
            actor, endpoint = next(iter(peers.items()))
            if endpoint["consumed"] is None:
                self.counts[origin + "_unconsumed_tail_posts"] += 1
                continue
            peer = key[2] if actor == key[1] else key[1]
            first, last = self.horizons.get(peer, {}).get(key[:-1], (0, 0))
            if origin == "native" and peer in self.horizons and key[-1] < first:
                self.counts["native_initial_external_completions"] += 1
            elif origin == "native" and peer in self.horizons and key[-1] >= last:
                self.terminals.append((key, endpoint["consumed"]))
                self.counts["native_terminal_external_completions"] += 1
            else:
                self.missing(origin + "_missing_peer_post", key=key, actor=actor)

    def native(self, native, dependencies):
        self.load_native()
        for (rank, thread), positions in self.native_threads.items():
            if rank in native and positions and all(event < 0 for points in positions.values() for _, event in points):
                self.missing("native_protocol_thread_has_no_coarse_anchor", rank=rank, thread=thread)

            # Equal clock readings do not identify an event or order two different events.
            previous = ()
            for _, current in sorted(positions.items()):
                self.edges["native"].update((source, target) for source in previous for target in current)
                previous = current
        self.connect_messages("native")
        if self.findings:
            return None
        graph = self.joined_graph("native", native, dependencies)

        # An unobserved suffix is harmless only if it cannot lead back to any retained serving event.
        relevant = [False] * len(graph.positions)
        for rank, original in native.items():
            for event in original.next:
                relevant[graph.positions[rank, event]] = True
        for position in range(len(relevant) - 1, -1, -1):
            relevant[position] |= any(relevant[target] for target in graph.successors[position])
        for key, consumed in self.terminals:
            if relevant[graph.positions[consumed]]:
                self.missing("native_terminal_message_can_reach_serving", key=key)
            else:
                self.counts["native_terminal_outside_serving"] += 1
        self.terminals_checked = True
        return graph

    def joined_graph(self, origin, graphs, dependencies):
        graph = CausalGraph.joined(graphs, dependencies)
        graph.add_events(self.points[origin])
        for source, target in self.edges[origin]:
            if source != target:
                graph.connect(source, target, "communication" if source[0] != target[0] else "control")
        graph.prepare()
        return graph

    def prediction(self, predicted, predicted_edges):
        if not self.predicted_scope:
            self.missing("missing_predicted_protocol_scope")
        self.connect_messages("predicted")

        # Keep each run's observed paths available; protocol completeness gates cross-run negative conclusions.
        return self.joined_graph("predicted", predicted, predicted_edges)

    def report(self):
        return {"complete": not self.findings and not self.returns and (not self.terminals or self.terminals_checked),
                "unreturned_actions": len(self.returns), **self.counts,
                "scope": "Actual CPU/Gloo post and successful consumption, run-local routes only. Native thread counters and actual predicted action returns anchor protocol paths. Prefix posts outside the window and unconsumed tails are explicit; no cross-run message pairing or CPU cost qualification."}
