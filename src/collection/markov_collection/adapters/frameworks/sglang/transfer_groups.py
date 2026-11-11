"""Close synchronous PD destination services at their shared chunk completion."""

import json
from collections import defaultdict

from markov_collection.adapters.runtimes.ascend.trace import AscendTrace


class TransferGroups:
    def __init__(self, recording):
        self.recording = recording
        self.members = defaultdict(list)
        for scope in recording["scopes"]:
            if "native_transfer" in scope:
                self.members[scope["submitted_by"]].append(scope["id"])
        for members in self.members.values():
            members.sort(key=lambda index: recording["scopes"][index]["host_interval"]["begin_counter"])

    def project(self, facts):
        scopes, spans = facts["scopes"], facts["spans"]
        by_scope = defaultdict(list)
        for index, span in enumerate(spans):
            by_scope[span["scope"]].append(index)
        aliases = {index: index for index in range(len(scopes))}
        removed_spans = set()
        for index, scope in enumerate(scopes):
            scope["bindings"]["source_scopes"] = [index]
        if not self.members:
            return facts

        for parent, members in self.members.items():
            owner, removed = self.close_group(facts, parent, members, by_scope)
            aliases.update({member: owner for member in members})
            removed_spans.update(removed)
        return self.compact(facts, aliases, removed_spans)

    def close_group(self, facts, parent, members, by_scope):
        scopes, spans = facts["scopes"], facts["spans"]
        raw = self.recording["scopes"]
        owned = {index for member in members for index in by_scope[member]}
        first, last = raw[members[0]], raw[members[-1]]
        begin, end = first["host_interval"]["begin_counter"], last["host_interval"]["end_counter"]
        if any(raw[index]["parent"] != parent or raw[index]["thread"] != first["thread"] for index in members):
            raise ValueError("PD destination group must have one submitting chunk and synchronous caller")
        if any(a["host_interval"]["end_counter"] > b["host_interval"]["begin_counter"]
               for a, b in zip((raw[i] for i in members), (raw[i] for i in members[1:]))):
            raise ValueError("PD destination services overlap within one synchronous group")
        if any(scope["parent"] in members for scope in scopes):
            raise ValueError("PD destination service contains another semantic call")

        resources = {facts["resources"][spans[index]["resource"]]["kind"]: spans[index]["resource"] for index in owned}
        if set(resources) != {"thread", "service"} or len({spans[i]["resource"] for i in owned}) != 2:
            raise ValueError("PD destination group has an undeclared shared resource")
        thread, service = resources["thread"], resources["service"]
        if any(index not in owned and span["resource"] in {thread, service} and span["begin"] < end and begin < span["end"]
               for index, span in enumerate(spans)):
            raise ValueError("PD destination group interleaves with another observed service")

        # Only the chunk's final completion may escape the closed service region.
        completion, = [port for port in scopes[parent]["ports"] if port["name"] == "transfer.complete"]
        for index, scope in enumerate(scopes):
            if index in members:
                continue
            for anchor in AscendTrace.anchors({"scopes": [scope], "dependencies": []}):
                if anchor["span"] in owned and anchor is not completion["at"]:
                    raise ValueError("PD service has a caller-visible intermediate boundary")
        incoming = []
        retained = []
        for edge in facts["dependencies"]:
            source, target = edge["source"]["span"] in owned, edge["target"]["span"] in owned
            if source and not target:
                raise ValueError("PD service has an outgoing intermediate dependency")
            if target and not source:
                if spans[edge["source"]["span"]]["scope"] != parent or edge["kind"] != "submission":
                    raise ValueError("PD service has an undeclared incoming dependency")
                incoming.append(edge["source"])
            if not source and not target:
                retained.append(edge)
        if not incoming or any(anchor != incoming[0] for anchor in incoming):
            raise ValueError("PD destination services do not share one enqueue boundary")

        owner = min(members)
        shared = ("parameters", "constants", "arguments", "guards")
        if any(scopes[member][field] != scopes[owner][field] for member in members for field in shared):
            raise ValueError("PD destination services disagree on chunk geometry or transformation")

        host_span, service_span = len(spans), len(spans) + 1
        spans.extend([
            {"scope": owner, "resource": thread, "begin": begin, "end": end, "kind": "wait"},
            {"scope": owner, "resource": service, "begin": begin, "end": end, "kind": "transfer"},
        ])
        host_begin, host_end = {"span": host_span, "order": begin}, {"span": host_span, "order": end}
        service_begin, service_end = {"span": service_span, "order": begin}, {"span": service_span, "order": end}
        retained.extend([
            {"source": incoming[0], "target": service_begin, "kind": "submission"},
            {"source": host_begin, "target": service_begin, "kind": "submission"},
            {"source": service_end, "target": host_end, "kind": "signal"},
        ])
        facts["dependencies"] = retained
        completion["at"] = service_end
        completion["signal"] = f"scope/{parent}/transfer.complete"
        combined = dict(scopes[owner])
        combined["work"] = {}
        for member in members:
            for key, value in scopes[member]["work"].items():
                if key in combined["work"] and combined["work"][key] != value:
                    raise ValueError("PD destination services disagree on logical work")
                combined["work"][key] = value
        combined.update(site="transfer_group", implementation="sglang.pd.transfer_group", bindings={"source_scopes": sorted(members)})
        combined["ports"] = [
            {"name": name, "direction": direction, "use": "control", "relation": "control", "at": at}
            for name, direction, at in (("call.enter", "input", host_begin), ("call.return", "output", host_end),
                                       ("service.complete", "output", service_end))
        ]
        for field in ("kv_accesses", "slot_accesses"):

            # Retain every logical slice/publication once; dispatch order does not define payload identity.
            accesses = {json.dumps({key: value for key, value in access.items() if key not in {"start", "finish"}}, sort_keys=True)
                        for member in members for access in scopes[member][field]}
            combined[field] = [{**json.loads(access), "start": service_begin, "finish": service_end} for access in sorted(accesses)]
        scopes[owner] = combined
        return owner, owned

    @staticmethod
    def compact(facts, aliases, removed_spans):
        scopes, spans = facts["scopes"], facts["spans"]

        # Raw scope numbers remain provenance only; all graph references use the compacted scope space.
        scope_ids = {old: new for new, old in enumerate(i for i in range(len(scopes)) if aliases[i] == i)}
        mapped = {old: scope_ids[owner] for old, owner in aliases.items()}
        facts["scopes"] = [scope for index, scope in enumerate(scopes) if aliases[index] == index]
        for scope in facts["scopes"]:
            if scope["parent"] is not None:
                scope["parent"] = mapped[scope["parent"]]
            for port in scope["ports"]:
                if tensor := port.get("tensor"):
                    for field in ("writer", "allocation"):
                        if tensor[field] is not None:
                            tensor[field] = mapped[tensor[field]]
        for span in spans:
            span["scope"] = mapped[span["scope"]]
        span_ids = {old: new for new, old in enumerate(i for i in range(len(spans)) if i not in removed_spans)}
        for anchor in {id(value): value for value in AscendTrace.anchors(facts)}.values():
            anchor["span"] = span_ids[anchor["span"]]
        facts["spans"] = [span for index, span in enumerate(spans) if index not in removed_spans]

        # Service payload is opaque across its closed interval; the caller thread remains waiting, not CPU work.
        services = {index for index, resource in enumerate(facts["resources"]) if resource["kind"] == "service"}
        for resource in services:
            facts["resources"][resource]["payload"] = sorted(
                [span["begin"], span["end"]] for span in facts["spans"] if span["resource"] == resource and span["begin"] < span["end"]
            )
        return facts
