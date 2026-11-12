"""Close private CPU continuations and synchronous PD destination services."""

import json
from collections import defaultdict

from markov_adapters.sglang.selection import BatchTransform
from markov_collection.adapters.runtimes.ascend.trace import AscendTrace


class ClosedRegions:
    def __init__(self, recording):
        self.recording = recording
        self.transfers = defaultdict(list)
        for scope in recording["scopes"]:
            if "native_transfer" in scope:
                self.transfers[scope["submitted_by"]].append(scope["id"])
        for members in self.transfers.values():
            members.sort(key=lambda index: recording["scopes"][index]["host_interval"]["begin_counter"])

    def normalize(self, facts):
        scopes, spans = facts["scopes"], facts["spans"]
        by_scope = defaultdict(list)
        for index, span in enumerate(spans):
            by_scope[span["scope"]].append(index)
        aliases = {index: index for index in range(len(scopes))}
        removed_spans = set()
        for index, scope in enumerate(scopes):
            scope["bindings"]["source_scopes"] = [index]
        for parent, members in self.transfers.items():
            owner, removed = self.close_group(facts, parent, members, by_scope)
            aliases.update({member: owner for member in members})
            removed_spans.update(removed)

        for members in self.control_groups(scopes):
            owner, removed = self.close_control(facts, members, by_scope)
            aliases.update({member: owner for member in members})
            removed_spans.update(removed)

        return self.compact(facts, aliases, removed_spans)

    @staticmethod
    def control_groups(scopes):
        children = defaultdict(list)
        for index, scope in enumerate(scopes):
            children[scope["parent"]].append(index)

        for siblings in children.values():
            position = 0
            while position + 2 < len(siblings):
                members = [siblings[position]]
                while position + 2 < len(siblings):
                    left, middle, right = (scopes[index] for index in siblings[position:position + 3])
                    transform = middle["bindings"].get("condition", {}).get("transformation", {})
                    if (left["role"] != "segment" or right["role"] != "segment" or middle["role"] != "batch_transform"
                            or not left["bindings"].get("segment", {}).get("host_only", False)
                            or not right["bindings"].get("segment", {}).get("host_only", False)
                            or not BatchTransform.private(transform.get("operation"), middle["parameters"]["left_members"])):
                        break
                    members.extend(siblings[position + 1:position + 3])
                    position += 2
                if len(members) > 1:
                    yield members
                position += 1

    def close_control(self, facts, members, by_scope):
        scopes, spans = facts["scopes"], facts["spans"]
        first, last = scopes[members[0]], scopes[members[-1]]
        owned = {span for member in members for span in by_scope[member]}
        intervals = [spans[span] for member in members for span in by_scope[member]]
        resource = intervals[0]["resource"]
        descriptor = facts["resources"][resource]
        if (len(intervals) != len(members) or descriptor["kind"] != "thread" or descriptor["role"] != "control"
                or any(span["kind"] != "host" or span["resource"] != resource for span in intervals)
                or any(a["end"] > b["begin"] for a, b in zip(intervals, intervals[1:]))):
            raise ValueError("private CPU region contains nonserial or nonhost work")
        if any(scope["parent"] in members for scope in scopes):
            raise ValueError("private CPU region contains a nested observable call")
        if any(edge[side]["span"] in owned for edge in facts["dependencies"] for side in ("source", "target")):
            raise ValueError("private CPU region has an externally used dependency")
        for index, scope in enumerate(scopes):
            if index not in members and any(anchor["span"] in owned for anchor in AscendTrace.anchors({"scopes": [scope], "dependencies": []})):
                raise ValueError("private CPU region exposes an intermediate boundary")

        for position, member in enumerate(members):
            scope = scopes[member]
            if (scope["kv_accesses"] or scope["slot_accesses"] or "communication" in scope or len(scope["ports"]) != 2
                    or any(port["use"] != "control" or port["name"] not in {"call.enter", "call.return"} for port in scope["ports"])):
                raise ValueError("private CPU region contains data, resource or synchronization effects")
            if position % 2 == 0:
                fields = ("parent", "site", "implementation", "parameters", "constants", "arguments", "guards")
                work = {key: value for key, value in scope["work"].items() if key != "continuation"}
                initial = {key: value for key, value in first["work"].items() if key != "continuation"}
                bindings = {key: value for key, value in scope["bindings"].items() if key != "source_scopes"}
                expected = {key: value for key, value in first["bindings"].items() if key != "source_scopes"}
                if (not bindings.get("segment", {}).get("host_only", False) or bindings != expected or work != initial
                        or any(scope[field] != first[field] for field in fields)):
                    raise ValueError("private CPU continuation changes its native owner or logical work")
            else:
                before, after = scopes[members[position - 1]]["work"]["continuation"], scopes[members[position + 1]]["work"]["continuation"]
                context = {"operation": "filter", "left": []}
                if (scope["bindings"]["condition"]["transformation"]["outcome"] != "unchanged"
                        or scope["work"]["context"] != {**context, "result": []}
                        or before["exit"] != "batch_transform" or after["entry"] != "batch_transform"
                        or before["exit_context"] != context or after["entry_context"] != context):
                    raise ValueError("private batch cleanup does not join its surrounding continuations")

        # Keep the measured outer CPU interval, including the native cleanup; only private cut points disappear.
        owner, host = members[0], len(spans)
        begin, end = intervals[0]["begin"], intervals[-1]["end"]
        spans.append({"scope": owner, "resource": resource, "begin": begin, "end": end, "kind": "host"})
        first["bindings"]["source_scopes"] = sorted(origin for member in members for origin in scopes[member]["bindings"]["source_scopes"])
        first["work"] = {**first["work"], "continuation": {
            **first["work"]["continuation"], **{field: last["work"]["continuation"][field] for field in ("exit", "exit_context")},
        }}
        first["ports"] = [
            {"name": name, "direction": direction, "use": "control", "relation": "control", "at": {"span": host, "order": order}}
            for name, direction, order in (("call.enter", "input", begin), ("call.return", "output", end))
        ]
        return owner, owned

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
