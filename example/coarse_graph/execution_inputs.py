"""Prepare native execution conditions and independently check their actual consumption."""

import argparse
import json
from collections import Counter, defaultdict
from pathlib import Path

from markov_adapters.pytorch import CpuTransfer

from actions import ActionIdentity
from ingress import IngressEvidence
from native_case import NativeCase


class NativeInputs:
    def __init__(self, capture, graphs, output):
        self.capture, self.graphs, self.output = capture, graphs, output
        self.steps = defaultdict(list)
        self.cpu_posts = defaultdict(dict)
        self.origins, self.ingress, self.windows = {}, {}, {}
        self.counts = Counter()

    @staticmethod
    def read(path):
        return json.loads(path.read_text())

    @staticmethod
    def window(begin, end, origin):
        if begin > end or end < origin:
            raise ValueError("native condition has a reversed or pre-workload interval")
        return {"begin_ns": max(begin - origin, 0), "end_ns": end - origin}

    @staticmethod
    def port(name, direction, window):
        return {"port": name, "direction": direction, "window": window}

    def actions(self, group, source, identities, clock, origin):
        graph, evidence = source.graph, source.graph["evidence"]
        if evidence["clock"] != "monotonic_raw_ns":
            raise ValueError("native input conditions require the common RAW clock")
        horizon = self.window(*evidence["window_ns"], origin)
        self.windows[group] = horizon
        self.counts["unmeasured_work_finishes"] += sum(evidence["events"][work["finish"]]["time_ns"] is None for work in graph["work"])
        for index, identity in identities.items():
            scope = graph["scopes"][index]
            ports = []
            for port in scope["ports"]:
                measured = clock.window(port["event"])
                if measured is None:

                    # Acquisition synchronizes device tails before closing this measured observation window.
                    condition = horizon
                    self.counts["ports_bounded_by_observation_window"] += 1
                else:
                    condition = self.window(*measured, origin)
                    self.counts["measured_ports"] += 1
                    self.counts["calibrated_port_windows"] += measured[0] != measured[1]
                ports.append(self.port(port["name"], port["direction"], condition))
            entered = next(port["event"] for port in scope["ports"] if port["name"] == "call.enter")
            begin = evidence["events"][entered]["time_ns"]
            if begin is None or "work" not in identity:
                raise ValueError("native action lacks its measured entry or complete logical identity")
            self.steps[group].append((begin, {"operation": "action", "identity": {
                name: identity[name] for name in ("action", "parameters", "work")
            }, "source": {"scope": index}, "service_ns": None, "ports": ports}, None))

    def input_reads(self, instance, rank, raw, aliases, requests, origin):
        if not raw["request_ingress"]:
            return
        name, group = instance["id"], f"{instance['id']}/{rank}"
        if name in self.ingress:
            raise ValueError("native input has more than one external ingress per instance")
        self.ingress[name] = IngressEvidence.rounds(raw["request_ingress"], aliases, requests, origin)
        if IngressEvidence.attempts(raw)["status"] != "observed":
            raise ValueError("native input reads lack their complete entering contexts")
        reads = IngressEvidence.reads(name, raw, aliases, origin)
        for number, (interval, read) in enumerate(zip(raw["request_input_attempts"], reads, strict=True)):
            self.steps[group].append((interval["begin_raw_ns"], {
                "operation": "input", "identity": read["identity"], "source": {"input": number},
                "window": {"begin_ns": read["time_ns"], "end_ns": read["time_ns"]},
            }, None))

    def cpu(self, instance, rank, origin):
        group = f"{instance['id']}/{rank}"
        document = self.read(self.capture / "protocol" / f"rank-{rank}.json")
        for post in document["posts"]:
            key = tuple(post["key"])
            if rank in self.cpu_posts[key]:
                raise ValueError("native CPU input repeats a local post")
            self.cpu_posts[key][rank] = {"post": post, "group": group, "origin": origin, "position": None}
            intervals = [(post["interval"], (key, rank))]
            if post["operation"] in {"isend", "irecv"} and post["consumed"] is not None:
                intervals.append((post["consumed"], None))
            for interval, position in intervals:
                execution = interval["execution"]
                if not execution["in_flow"]:
                    continue
                work = execution["work"]
                context = work["context"]
                work = {**work, "context": {**context, "cpu_transfer": CpuTransfer.identity(context["cpu_transfer"])}}
                begin, end = interval["begin_raw_ns"], interval["end_raw_ns"]
                self.steps[group].append((begin, {
                    "operation": "cpu", "identity": {
                        "operation": execution["operation"], "work": ActionIdentity.work_identity(work),
                    }, "source": {"post": list(key), "phase": "post" if position is not None else "wait"}, "service_ns": None, "ports": [
                        self.port("call.enter", "input", self.window(begin, begin, origin)),
                        self.port("call.return", "output", self.window(end, end, origin)),
                    ],
                }, position))

    def transfers(self):
        result = []
        for peers in self.cpu_posts.values():
            if len(peers) != 2:
                continue
            consumed = [peer["post"]["consumed"]["end_raw_ns"] for peer in peers.values() if peer["post"]["consumed"] is not None]
            positions = sorted((peer["position"] for peer in peers.values() if peer["position"] is not None), key=lambda row: (row["group"], row["step"]))
            if not consumed or not positions:
                continue
            origins = {peer["origin"] for peer in peers.values()}
            if len(origins) != 1:
                raise ValueError("native CPU message crosses unrelated workload clocks")
            origin, = origins
            begin = max(peer["post"]["interval"]["begin_raw_ns"] for peer in peers.values())
            result.append({"posts": positions, "service_ns": None, "window": self.window(begin, min(consumed), origin)})
        return result

    def run(self):
        self.output.mkdir(parents=True)
        manifest = self.read(self.capture / "capture.json")
        evidence = NativeCase(self.capture, manifest)
        report = {"capture": str(self.capture.resolve()), "graphs": str(self.graphs.resolve()), "status": "unverified"}
        try:
            for number, instance in enumerate(manifest["instances"]):
                directory = self.capture / "instances" / str(number)
                session = self.read(directory / "session.json")
                delivery, = self.read(directory / "request-delivery.json")["capture"]
                if delivery["clock"] != "monotonic_raw_ns" or set(instance["observed_roles"]) != set(session["native_participants"]):
                    raise ValueError("native input needs the common clock and every actual participant")
                origin = delivery["origin_ns"]
                requests = {request["id"]: request for request in session["content"]["requests"]}
                aliases = dict(zip(session["source_requests"], requests, strict=True))
                for rank in session["native_participants"]:
                    group = f"{instance['id']}/{rank}"
                    self.origins[group] = origin
                    raw = self.read(self.capture / f"rank-{rank}.json")
                    graph = self.read(self.graphs / f"{rank}-graph.json")
                    source = evidence.observe(rank, graph, raw)
                    self.actions(group, source, evidence.sources[rank][2], evidence.clocks[rank], origin)
                    self.input_reads(instance, rank, raw, aliases, requests, origin)
                    self.cpu(instance, rank, origin)
                if instance["id"] not in self.ingress:
                    raise ValueError("native case omits external ingress")
            if len(set(self.origins.values())) != 1:
                raise ValueError("native serving instances have different workload origins")
            evidence.finish()
            native_report = evidence.report(False)
            if native_report["status"] == "differences":
                raise ValueError("native source has confirmed contradictions")
            actors = {}
            for group in self.origins:
                ordered = sorted(self.steps[group], key=lambda step: step[0])
                if len({begin for begin, _, _ in ordered}) != len(ordered):
                    raise ValueError(f"native {group} has unordered operation entries at the same RAW timestamp")
                actors[group] = {"steps": [step for _, step, _ in ordered], "unmeasured_completion_window": self.windows[group]}
                for position, (_, _, post) in enumerate(ordered):
                    if post is not None:
                        key, rank = post
                        self.cpu_posts[key][rank]["position"] = {"group": group, "step": position}
            notifications = [
                {"identity": {"group": row["group"], "name": name, "work": {"request": row["request"]}},
                 "window": self.window(row[field]["begin_raw_ns"], row[field]["end_raw_ns"], self.origins[row["group"]])}
                for row in evidence.readiness["endpoints"]
                for name, field in (("kv_ready", "reservation_update"), ("kv_success", "completion_update"))
                if row[field] is not None and (name != "kv_ready" or row["remote_reservation"])
            ]
            document = {"actors": actors, "ingress": self.ingress, "notifications": notifications, "transfers": self.transfers()}
            report.update(status="partial" if self.counts["ports_bounded_by_observation_window"] or self.counts["unmeasured_work_finishes"]
                          or self.counts["calibrated_port_windows"]
                          or native_report["status"] != "consistent" else "extracted",
                          native_evidence=native_report, counts=dict(self.counts), steps={group: len(actor["steps"]) for group, actor in actors.items()})
            (self.output / "execution-inputs.json").write_text(json.dumps(document) + "\n")
        except (OSError, ValueError, KeyError, IndexError, TypeError) as error:
            report["error"] = str(error)
        report["scope"] = "Checked actor operations and measured or explicit bounded time conditions. Device windows preserve a common feasible calibration envelope; separate windows do not prove one joint clock realization. No batches, query results, allocations or graph bodies are supplied. Broad observation windows retain uncertainty; no cost accuracy or unique native trajectory qualification."
        (self.output / "execution-inputs-report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
        return 0 if report["status"] == "extracted" else 1


class ConsumedInputs:
    """Prove consumed prefixes from native observations, independently of the supplied condition file."""

    def __init__(self, case, ingress):
        self.capture, self.groups, self.ingress = case.capture, case.groups, ingress
        self.sources = dict(case.sources)
        self.expected = None
        self.positions = Counter()
        self.pending, self.reads = {}, {}
        self.input_pairs = defaultdict(dict)
        self.counts = Counter()

    def prepare(self):
        ordered = defaultdict(list)
        for rank, (source, raw, identities) in self.sources.items():
            group = self.groups[rank]

            # Original CPU entries establish order; graph scope references retain their independently checked raw origins.
            host = {int(row["name"].split("/")[1]): row["begin_raw_ns"]
                    for row in raw["host_observation"]["intervals"] if row["name"].startswith("markov.scope/")}
            for index, identity in identities.items():
                origins = source.graph["scopes"][index]["bindings"]["source_scopes"]
                begin = min(raw["scopes"][origin]["host_interval"]["begin_raw_ns"] if "service" in raw["scopes"][origin] else host[origin]
                            for origin in origins)
                ordered[group].append((begin, {"operation": "action", "identity": {key: identity[key] for key in ("action", "parameters", "work")},
                                              "source": {"scope": index}}))

            protocol = json.loads((self.capture / f"protocol/rank-{rank}.json").read_text())
            for post in protocol["posts"]:
                for phase, interval in (("post", post["interval"]), ("wait", post["consumed"])):
                    if interval is None or not interval["execution"]["in_flow"] or (phase == "wait" and post["operation"] not in {"isend", "irecv"}):
                        continue
                    execution = interval["execution"]
                    work = execution["work"]
                    work = {**work, "context": {**work["context"], "cpu_transfer": CpuTransfer.identity(work["context"]["cpu_transfer"])}}
                    ordered[group].append((interval["begin_raw_ns"], {
                        "operation": "cpu", "identity": {"operation": execution["operation"], "work": ActionIdentity.work_identity(work)},
                        "source": {"post": post["key"], "phase": phase},
                    }))

            if raw["request_ingress"]:
                instance = group.rsplit("/", 1)[0]
                for index, (interval, read) in enumerate(zip(raw["request_input_attempts"], self.ingress.native_reads[instance], strict=True)):
                    ordered[group].append((interval["begin_raw_ns"], {
                        "operation": "input", "identity": read["identity"], "source": {"input": index},
                    }))

        self.expected = {}
        for group, records in ordered.items():
            records.sort(key=lambda item: item[0])
            if any(left[0] >= right[0] for left, right in zip(records, records[1:])):
                raise ValueError("native condition witnesses need strictly ordered measured CPU entries")
            self.expected[group] = [record for _, record in records]
        self.sources.clear()

    def observe(self, row):
        if row["kind"] == "condition_used":
            if self.expected is None:
                self.prepare()
            group = row["group"]
            position = self.positions[group]
            record = {key: row[key] for key in ("operation", "identity", "source")}
            if (type(row["step"]) is not int or row["step"] != position or position >= len(self.expected[group])
                    or record != self.expected[group][position] or group in self.pending):
                raise ValueError(f"consumed execution condition differs from its native causal prefix: {group} at {position}")
            self.positions[group] += 1
            self.counts[record["operation"]] += 1
            if record["operation"] == "input":
                instance = record["identity"]["instance"]
                if instance in self.reads:
                    raise ValueError("input condition was consumed before the preceding read returned")
                self.reads[instance] = record["source"]["input"], record["identity"]
            else:
                self.pending[group] = record
        elif row["kind"] == "input_read" and self.expected is not None:
            instance = row["identity"]["instance"]
            if instance in self.reads:
                source, identity = self.reads.pop(instance)
            elif not row["in_flow"]:
                return
            else:
                raise ValueError("active input read has no consumed condition")
            if identity != row["identity"]:
                raise ValueError("input result belongs to a different consumed condition")
            self.input_pairs[instance][len(self.ingress.actual_reads[instance])] = source

    def graph(self, group, root):
        if self.expected is None:
            return None
        record = self.pending.pop(group)
        if root["role"] == "cpu_service":
            execution = root["bindings"]["cpu_protocol"]["execution"]
            operation = "cpu"
            identity = {"operation": execution["operation"], "work": ActionIdentity.work_identity(root["work"])}
        else:
            operation = "action"
            identity = {key: value for key, value in ActionIdentity.record(root, group).items() if key != "group"}
        if record["operation"] != operation or record["identity"] != identity:
            raise ValueError("instantiated graph differs from its consumed execution condition")
        return record["source"]["scope"] if operation == "action" else None

    def finish(self):
        if self.expected is None:
            return {"status": "not_used", "verified_prefix": False,
                    "scope": "No consumed-condition witness; ordinary logical pairing only."}
        remaining = sum(len(records) - self.positions[group] for group, records in self.expected.items())
        return {"status": "complete" if not remaining and not self.pending and not self.reads else "partial", "verified_prefix": True,
                "checked": dict(self.counts), "unconsumed": remaining, "unbound_graphs": len(self.pending), "unreturned_reads": len(self.reads),
                "scope": "Every consumed operation, full work identity and native reference checked in strict observed CPU order. Action references bind actual instantiated graphs. No cost accuracy or independent target generalization claim."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", type=Path, required=True)
    parser.add_argument("--graphs", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    raise SystemExit(NativeInputs(args.capture, args.graphs, args.output).run())


if __name__ == "__main__":
    main()
