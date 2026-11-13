"""Close private CPU continuations without changing externally used boundaries."""

from collections import defaultdict

from markov_adapters.sglang.selection import BatchTransform
from markov_collection.instrumentation.observation import ObservationReferences


class ClosedRegions:
    def normalize(self, facts):
        scopes, spans = facts["scopes"], facts["spans"]
        by_scope = defaultdict(list)
        for index, span in enumerate(spans):
            by_scope[span["scope"]].append(index)
        aliases = {index: index for index in range(len(scopes))}
        removed_spans = set()
        for index, scope in enumerate(scopes):
            scope["bindings"]["source_scopes"] = [index]

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
            if index not in members and any(anchor["span"] in owned for anchor in ObservationReferences({"scopes": [scope], "dependencies": []})):
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
        for anchor in {id(value): value for value in ObservationReferences(facts)}.values():
            anchor["span"] = span_ids[anchor["span"]]
        facts["spans"] = [span for index, span in enumerate(spans) if index not in removed_spans]

        # Service payload is opaque across its closed interval; the caller thread remains waiting, not CPU work.
        services = {index for index, resource in enumerate(facts["resources"]) if resource["kind"] == "service"}
        for resource in services:
            facts["resources"][resource]["payload"] = sorted(
                [span["begin"], span["end"]] for span in facts["spans"] if span["resource"] == resource and span["begin"] < span["end"]
            )
        return facts
