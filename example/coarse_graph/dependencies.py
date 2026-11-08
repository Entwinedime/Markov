"""Compare causal relationships without pairing opaque event numbers or measured times."""

from collections import Counter, defaultdict

from actions import ActionIdentity


class ActionBoundaries:
    def __init__(self, contents, root, event_ids=None):
        graph = contents.graph
        event_ids = range(len(graph["events"])) if event_ids is None else event_ids
        paths, resources = contents.paths(root), {}
        self.events, self.work, self.resources = {}, [], {}
        for scope, path in paths.items():
            node = graph["scopes"][scope]
            for port in sorted(node["ports"], key=lambda value: (value["name"], value["direction"])):

                # Generated action frontiers have no source port; their edges still participate in reachability.
                if scope == root and port["name"].startswith(("resource.enter/", "resource.exit/")):
                    continue
                name = f"{path}/{port['direction']}:{port['name']}"
                self.events[name] = event_ids[port["event"]]
                resource = graph["events"][port["event"]]["resource"]
                resources.setdefault(resource, len(resources))
                if "wait" in port:
                    self.events[name + "/wait.begin"] = event_ids[port["wait"]["begin"]]
                    resources.setdefault(port["wait"]["resource"], len(resources))
                if "rendezvous" in port:
                    self.events[name + "/rendezvous"] = event_ids[port["rendezvous"]]
            for field in ("kv_accesses", "slot_accesses"):
                for index, access in enumerate(node[field]):
                    for side in ("start", "finish"):
                        self.events[f"{path}/{field}/{index}/{side}"] = event_ids[access[side]]

        for scope, path in paths.items():
            for work in contents.work[scope]:
                resource = work["resource"]
                descriptor = contents.resource(resource)
                self.resources[resource] = (*descriptor.values(), resources.get(resource))
                identity = (path, work["kind"], resource)
                self.work.append((identity, event_ids[work["start"]], event_ids[work["finish"]]))


class CausalGraph:
    relations = ("order", "submission", "data", "signal", "control", "state", "communication")

    def __init__(self):
        self.next = {}

    @classmethod
    def observed(cls, graph):
        result = cls()
        result.add_events(range(len(graph["events"])))
        for edge in graph["edges"]:
            result.connect(edge["source"], edge["target"], edge["kind"])
        for work in graph["work"]:
            result.connect(work["start"], work["finish"], "order")
        result.prepare()
        return result

    def add_events(self, events):
        for event in events:
            if event in self.next:
                raise ValueError("execution repeats an event identity")
            self.next[event] = {}

    def connect(self, source, target, relation):
        if source not in self.next or target not in self.next:
            raise ValueError("dependency refers to an undeclared event")
        edges = self.next[source]
        edges[target] = edges.get(target, 0) | (1 << self.relations.index(relation))

    def prepare(self):
        pending = dict.fromkeys(self.next, 0)
        for targets in self.next.values():
            for target in targets:
                pending[target] += 1
        ordered = [event for event, count in pending.items() if not count]
        for event in ordered:
            for target in self.next[event]:
                pending[target] -= 1
                if not pending[target]:
                    ordered.append(target)
        if len(ordered) != len(self.next):
            raise ValueError("dependency graph contains a cycle")
        self.positions = {event: index for index, event in enumerate(ordered)}
        self.successors = [[self.positions[target] for target in self.next[event]] for event in ordered]

    def signature(self, action):
        names = sorted(action.events)
        anchors = [self.positions[action.events[name]] for name in names]
        work = [(identity, self.positions[start], self.positions[finish]) for identity, start, finish in action.work]
        positions = anchors + [event for _, start, finish in work for event in (start, finish)]
        begin, end = min(positions), max(positions) + 1
        before, after = [0] * (end - begin), [0] * (end - begin)
        for bit, position in enumerate(anchors):
            before[position - begin] |= 1 << bit
            after[position - begin] |= 1 << bit

        # Paths may leave the action tree. Only the topological interval, not event ownership, bounds the search.
        for source in range(begin, end):
            for target in self.successors[source]:
                if target < end:
                    before[target - begin] |= before[source - begin]
        for source in range(end - 1, begin - 1, -1):
            for target in self.successors[source]:
                if target < end:
                    after[source - begin] |= after[target - begin]

        boundaries = {name: before[position - begin] for name, position in zip(names, anchors, strict=True)}
        placement = defaultdict(Counter)
        for (scope, kind, resource), start, finish in work:
            placement[resource][(scope, kind, before[start - begin], before[finish - begin], after[start - begin], after[finish - begin])] += 1

        # Private submission threads need no public port. Compare their work as resource groups, not by thread ID.
        placement = Counter((action.resources[resource], frozenset(items.items())) for resource, items in placement.items())
        return BoundarySignature(names, boundaries, placement)

    def reaches(self, source, target):
        start, finish = self.positions[source], self.positions[target]
        pending, visited = [start], {start}
        while pending:
            current = pending.pop()
            if current == finish:
                return True
            for successor in self.successors[current]:
                if successor <= finish and successor not in visited:
                    visited.add(successor)
                    pending.append(successor)
        return False

    def witness(self, source, target):
        pending, parents = [source], {source: None}
        finish = self.positions[target]
        for event in pending:
            if event == target:
                break
            for successor in sorted(self.next[event]):
                if self.positions[successor] <= finish and successor not in parents:
                    parents[successor] = event
                    pending.append(successor)
        if target not in parents:
            raise ValueError("reported ordering difference has no supporting path")

        counts, path = Counter(), []
        event = target
        while (previous := parents[event]) is not None:
            relations = [name for bit, name in enumerate(self.relations) if self.next[previous][event] & (1 << bit)]
            counts.update(relations)
            path.append({"source": previous, "target": event, "relations": relations})
            event = previous
        path.reverse()

        # One actual path explains the finding; it does not prove that other paths or state effects are absent.
        return {"edges": len(path), "relations": dict(counts), "first_edges": path[:3], "last_edges": path[-3:]}

    def reachable_masks(self, origins):
        masks = [0] * len(self.positions)
        for bit, event in enumerate(origins):
            masks[self.positions[event]] |= 1 << bit
        for source, targets in enumerate(self.successors):
            if masks[source]:
                for target in targets:
                    masks[target] |= masks[source]
        return masks

    def time_conflict(self, times):
        latest = [None] * len(self.positions)
        for event, position in self.positions.items():
            predecessor = latest[position]
            if event in times:
                if predecessor is not None and predecessor[0] > times[event]:
                    return predecessor[1], event
                predecessor = times[event], event

            # Propagate through unmeasured nodes; endpoint-only checks miss conflicts across hidden paths.
            if predecessor is not None:
                for target in self.successors[position]:
                    if latest[target] is None or predecessor[0] > latest[target][0]:
                        latest[target] = predecessor
        return None


class ActionOrderComparison:
    def __init__(self):
        self.counts, self.findings = Counter(), []
        self.repeated_counts = Counter()

    @staticmethod
    def frontiers(graph, origins, events):
        result = {event: [0, 0] for event in events}

        # Retain full masks only at inspected calls; propagation stays bounded like ordinary order comparison.
        for begin in range(0, len(origins), 1024):
            current = origins[begin:begin + 1024]
            before = graph.reachable_masks(current)
            after = [0] * len(graph.positions)
            for bit, event in enumerate(current):
                after[graph.positions[event]] |= 1 << bit
            for source in range(len(after) - 1, -1, -1):
                for target in graph.successors[source]:
                    after[source] |= after[target]
            for event, masks in result.items():
                position = graph.positions[event]
                masks[0] |= before[position] << begin
                masks[1] |= after[position] << begin
        return result

    def compare_repeated(self, actual, predicted, actions, repeated):
        if not repeated:
            return
        names = ("root/input:call.enter", "root/output:call.return")
        anchors = [(native.events[name], target.events[name]) for _, native, target in actions for name in names]
        if not anchors:
            self.repeated_counts["unqualified_contexts"] += len(repeated)
            self.repeated_counts["unqualified_calls"] += sum(len(targets) for _, _, targets in repeated.values())
            return
        frontiers = []
        for side, graph in enumerate((actual, predicted)):
            events = set()
            for row in repeated.values():
                for call in row[side + 1]:
                    events.update(call.events[name] for name in names)
            frontiers.append(self.frontiers(graph, [row[side] for row in anchors], events))
        for identity, native, targets in repeated.values():
            if not native:
                self.repeated_counts.update(unqualified_contexts=1, unqualified_calls=len(targets))
                continue
            positions = [{tuple(value for name in names for value in masks[call.events[name]]) for call in calls}
                         for masks, calls in zip(frontiers, (native, targets), strict=True)]
            missing, extra = positions[0] - positions[1], positions[1] - positions[0]
            self.repeated_counts.update(contexts_checked=1, native_positions=len(positions[0]), predicted_positions=len(positions[1]),
                                        matched_positions=len(positions[0] & positions[1]), calls_checked=len(targets))
            if not missing and not extra:
                self.repeated_counts.update(contexts_matched=1, calls_matched=len(targets))
                continue

            # Compare whole position sets. No member is selected to supply a source occurrence, body or duration.
            examples = {}
            for label, differences, calls, masks in (("native", missing, native, frontiers[0]), ("predicted", extra, targets, frontiers[1])):
                for call in calls:
                    signature = tuple(value for name in names for value in masks[call.events[name]])
                    if signature in differences:
                        examples[label] = {"events": {name: call.events[name] for name in names},
                                           "anchor_counts": [mask.bit_count() for mask in signature]}
                        break
            self.findings.append(("repeated_call_order", identity, {"native_only_positions": len(missing),
                                  "predicted_only_positions": len(extra), "examples": examples}))

    def compare(self, actual, predicted, actions):
        boundaries, identities = {}, {}
        for identity, native, estimate in actions:
            if native.events.keys() != estimate.events.keys():
                self.findings.append(("ordering_evidence", identity, {"reason": "action boundary sets differ"}))
                return
            key = ActionIdentity.occurrence_key(identity)
            identities[key] = identity
            for name, event in native.events.items():
                boundaries[key, name] = event, estimate.events[name]
        labels = sorted(boundaries)
        positions = [(actual.positions[boundaries[label][0]], predicted.positions[boundaries[label][1]]) for label in labels]
        differences = {}

        # Bound per-event masks to 1024 bits; do not retain a quadratic whole-case reachability table.
        for begin in range(0, len(labels), 1024):
            origins = labels[begin : begin + 1024]
            native_masks = actual.reachable_masks([boundaries[label][0] for label in origins])
            predicted_masks = predicted.reachable_masks([boundaries[label][1] for label in origins])
            local = defaultdict(int)
            for bit, (key, _) in enumerate(origins):
                local[key] |= 1 << bit
            for label, (native_position, predicted_position) in zip(labels, positions, strict=True):

                # Within-action order already has its own comparison; these masks contain only other actions.
                mask = ~local[label[0]]
                native = native_masks[native_position] & mask
                estimate = predicted_masks[predicted_position] & mask
                self.counts["predecessor_pairs_expected"] += native.bit_count()
                self.counts["predecessor_pairs_matched"] += (native & estimate).bit_count()
                if native == estimate:
                    continue
                detail = differences.setdefault(label, {
                    "boundary": label[1], "missing_predecessors": 0, "extra_predecessors": 0,
                    "missing_examples": [], "extra_examples": [],
                })
                for kind, missing in (("missing", native & ~estimate), ("extra", estimate & ~native)):
                    detail[kind + "_predecessors"] += missing.bit_count()
                    self.counts[kind + "_predecessor_pairs"] += missing.bit_count()
                    examples = detail[kind + "_examples"]
                    while missing and len(examples) < 3:
                        bit = missing & -missing
                        key, name = origins[bit.bit_length() - 1]
                        side, graph = (0, actual) if kind == "missing" else (1, predicted)
                        path = graph.witness(boundaries[key, name][side], boundaries[label][side])
                        examples.append({"work": identities[key], "boundary": name, "path": path})
                        missing ^= bit
        self.counts.update(
            groups_compared=1, actions_compared=len(identities), boundaries_compared=len(labels),
            actions_matched=len(identities) - len({key for key, _ in differences}),
            boundaries_matched=len(labels) - len(differences),
        )
        for (key, _), detail in differences.items():
            self.findings.append(("cross_action_order", identities[key], detail))


class BoundarySignature:
    def __init__(self, names, predecessors, work):
        self.names, self.predecessors, self.work = names, predecessors, work

    def explain_work(self, group):
        resource, work = group
        signature, count = min(work)
        scope, kind, *masks = signature
        result = {"resource": resource, "work_count": sum(count for _, count in work),
                  "example": {"scope": scope, "kind": kind, "count": count}}
        for field, mask in zip(("before_start", "before_finish", "after_start", "after_finish"), masks, strict=True):
            result["example"][field] = {"count": mask.bit_count(), "examples": self.labels(mask, 3)}
        return result

    def labels(self, mask, limit):
        result = []
        while mask and len(result) < limit:
            bit = mask & -mask
            result.append(self.names[bit.bit_length() - 1])
            mask ^= bit
        return result

    def differences(self, predicted):
        if self.names != predicted.names:
            yield "boundary_set", {"native_only": sorted(set(self.names) - set(predicted.names)),
                                   "predicted_only": sorted(set(predicted.names) - set(self.names))}
            return
        for target in self.names:
            native, estimate = self.predecessors[target], predicted.predecessors[target]
            if native != estimate:
                yield "boundary_order", {
                    "target": target, "missing_predecessors": (native & ~estimate).bit_count(),
                    "extra_predecessors": (estimate & ~native).bit_count(),
                    "missing_examples": self.labels(native & ~estimate, 3), "extra_examples": self.labels(estimate & ~native, 3),
                }
        if self.work != predicted.work:
            missing, extra = self.work - predicted.work, predicted.work - self.work
            yield "work_placement", {
                "native_resource_groups": sum(missing.values()), "predicted_resource_groups": sum(extra.values()),
                "native_example": self.explain_work(next(iter(missing))) if missing else None,
                "predicted_example": predicted.explain_work(next(iter(extra))) if extra else None,
            }
