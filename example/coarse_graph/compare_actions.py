"""Compare action contents, causal boundaries and data/communication connections with a native case."""

import argparse
import json
from collections import Counter, defaultdict
from pathlib import Path

from actions import ActionIdentity, ActionScopes, QueryResults
from bindings import BindingComparison
from ingress import IngressEvidence
from connections import Connections
from dependencies import ActionBoundaries, ActionOrderComparison, CausalGraph
from resource_effects import ResourceEffectComparison
from protocol_reads import PdPollingEvidence, PdStateEvidence, PdPredictionEvidence
from state_values import ActionResults, StateValueComparison


class GraphContents(ActionScopes):
    def __init__(self, graph):
        super().__init__(graph)
        self.pools = {}

    def observed_pools(self, raw, registrations, groups):

        # The native session identifies the actual destination independently of the predictor's routing rules.
        for index, observed in enumerate(raw["scopes"]):
            if not (transfer := observed.get("native_transfer")):
                continue
            target = groups[registrations[transfer["session"]]]
            scope = self.graph["scopes"][index]
            accesses = scope["kv_accesses"] + scope["slot_accesses"]
            if not accesses and not transfer["blocks"]:
                continue
            destinations = {access["pool"] for access in accesses if access["access"] == "write"}
            if len(destinations) != 1 or "local" in destinations:
                raise ValueError("native transfer has no unique remote graph pool")
            pool, = destinations
            if self.pools.setdefault(pool, target) != target:
                raise ValueError("source pool interface identifies different native owners")

    def pool(self, name, group):
        return group if name == "local" else self.pools.get(name, name)

    def resource(self, index):
        resource = self.graph["resources"][index]
        return {name: resource[name] for name in ("kind", "role", "rank", "serial")}

    def paths(self, root):
        paths, pending = {root: "root"}, [root]
        for index in pending:
            for child, scope in enumerate(self.children[index]):
                paths[scope] = f"{paths[index]}/{child}"
                pending.append(scope)
        return paths

    def snapshot(self, root, group):
        graph, paths = self.graph, self.paths(root)

        # Relative versions and alias partitions survive different addresses and allocation histories.
        versions = {}
        for index in paths:
            for port in graph["scopes"][index]["ports"]:
                if port["use"] == "data" and (tensor := port.get("tensor")):
                    storage = tensor["storage"]
                    versions[storage] = min(versions.get(storage, tensor["version"]), tensor["version"])
        storages, resources, channels = {}, {}, {}
        channel_origins = {}
        for index in paths:
            if communication := graph["scopes"][index].get("communication"):
                channel = communication["channel"]
                channel_origins[channel] = min(channel_origins.get(channel, communication["order"]), communication["order"])

        result = {}
        for index in paths:
            scope = graph["scopes"][index]
            contents = {name: scope[name] for name in ("role", "site", "implementation", "parameters")}
            contents["weights"] = scope["bindings"].get("weights")
            ports = {}
            for port in sorted(scope["ports"], key=lambda value: (value["name"], value["direction"])):

                # These ports are added during action cutting; cross-action serial frontiers need their own check.
                if index == root and port["name"].startswith(("resource.enter/", "resource.exit/")):
                    continue
                boundary = {name: port[name] for name in ("use", "relation")}
                resource = graph["events"][port["event"]]["resource"]
                resources.setdefault(resource, len(resources))
                boundary["resource"] = {**self.resource(resource), "alias": resources[resource]}
                if "wait" in port:
                    waited = port["wait"]["resource"]
                    resources.setdefault(waited, len(resources))
                    boundary["wait_resource"] = {**self.resource(waited), "alias": resources[waited]}
                boundary["rendezvous"] = "rendezvous" in port
                if tensor := port.get("tensor"):
                    boundary["tensor"] = {"dtype": tensor["dtype"], "shape": tensor["shape"]}

                    # Metadata describes layout; its runtime buffer lifecycle is not a graph data dependency.
                    if port["use"] == "data":
                        storage = tensor["storage"]
                        storages.setdefault(storage, len(storages))
                        boundary["tensor"].update(
                            storage_alias=storages[storage], version=tensor["version"] - versions[storage],
                            writer=paths.get(tensor["writer"], "external"),
                            allocation="internal" if tensor["allocation"] in paths else "external",
                            buffer=tensor["buffer"],
                        )
                ports[f"{port['direction']}:{port['name']}"] = boundary
            contents["ports"] = ports

            for field in ("kv_accesses", "slot_accesses"):
                accesses = []
                for access in scope[field]:
                    value = {name: value for name, value in access.items() if name not in {"start", "finish", "rule"}}
                    value["pool"] = self.pool(value["pool"], group)
                    accesses.append(value)
                contents[field] = accesses
            communication = scope.get("communication")
            if communication:
                channel = communication["channel"]
                channels.setdefault(channel, len(channels))
                communication = {**communication, "channel": channels[channel], "order": communication["order"] - channel_origins[channel]}
            contents["communication"] = communication
            work = Counter(json.dumps({"kind": item["kind"], "resource": self.resource(item["resource"])}, sort_keys=True)
                           for item in self.work[index])
            contents["work"] = dict(work)
            result[paths[index]] = contents
        return result


class ActionComparison:
    evidence_categories = frozenset({
        "native_evidence", "binding_evidence", "ordering_evidence", "input_read_evidence",
        "missing_native_identity", "missing_predicted_identity",
        "ambiguous_native_identity", "ambiguous_predicted_identity",
        "unpaired_native_context", "unpaired_predicted_context",
        "incomplete_execution", "unfinished_events",
    })

    def __init__(self, capture, graphs, execution, output, details):
        self.capture, self.graphs, self.execution, self.output, self.details = capture, graphs, execution, output, details
        self.counts, self.findings = Counter(), []

    @classmethod
    def differences(cls, native, predicted, path=""):
        if isinstance(native, dict) and isinstance(predicted, dict):
            for key in sorted(native.keys() | predicted.keys()):
                if key in native and key in predicted:
                    yield from cls.differences(native[key], predicted[key], f"{path}/{key}")
                else:
                    yield {"field": f"{path}/{key}", "native": native.get(key), "predicted": predicted.get(key)}
        elif native != predicted:
            yield {"field": path, "native": native, "predicted": predicted}

    def finding(self, category, identity, detail):
        self.counts[category] += 1
        if self.details or self.counts[category] == 1:
            kind = "evidence_gap" if category in self.evidence_categories else "difference"
            self.findings.append({"category": category, "kind": kind, "work": identity, **detail})

    def outcome(self, incomplete):
        categories = {category for category, count in self.counts.items() if count}
        incomplete = incomplete or bool(categories & self.evidence_categories)

        # Missing evidence must not hide a difference already established by another check.
        return {
            "status": "differences" if categories - self.evidence_categories else "incomplete" if incomplete else "actions_matched",
            "coverage": "partial" if incomplete else "checked_scope",
        }

    def compare_contents(self, identity, actual, predicted):
        different = False
        for path in sorted(actual.keys() | predicted.keys()):
            if path not in actual or path not in predicted:
                self.finding("scope_tree", identity, {"scope": path, "native": path in actual, "predicted": path in predicted})
                different = True
                continue
            for field in actual[path]:
                if actual[path][field] != predicted[path][field]:
                    changes = list(self.differences(actual[path][field], predicted[path][field]))
                    self.finding(field, identity, {"scope": path, "changes": changes if self.details else changes[:3]})
                    different = True
        return not different

    def publish(self, report):
        self.output.mkdir(parents=True)
        (self.output / "actions.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
        return 0 if report["status"] == "actions_matched" else 1

    def run(self):
        capture = json.loads((self.capture / "capture.json").read_text())
        groups = {rank: f"{instance['id']}/{rank}" for instance in capture["instances"] for rank in instance["observed_roles"]}
        ranks, expected, actual_dependencies, predicted_dependencies = {}, {}, {}, {}
        sources, occurrences, full_occurrences = {}, defaultdict(list), defaultdict(list)
        native_reads = {}
        protocol_source = PdStateEvidence()
        native_polling = PdPollingEvidence(protocol_source.admission_members)
        state_values = StateValueComparison(groups)
        action_results = ActionResults(groups)
        ingress = IngressEvidence(self.capture, capture["instances"])
        observed_actions = 0
        for instance in capture["instances"]:
            for rank in instance["observed_roles"]:
                ranks[rank] = instance["id"]
                graph = json.loads((self.graphs / f"{rank}-graph.json").read_text())
                source = GraphContents(graph)
                raw = json.loads((self.capture / f"rank-{rank}.json").read_text())
                source.validate_source(raw, rank)
                protocol_source.observe(raw, groups[rank])
                native_polling.native(raw, groups[rank])
                state_values.source(raw, groups[rank])
                action_results.source(raw, groups[rank], instance["mode"])
                ingress.native(instance["id"], raw)
                identities = {}
                sources[rank] = source, raw, identities
                for index, scope in enumerate(graph["scopes"]):
                    if not ActionIdentity.selected(index, scope):
                        continue
                    observed_actions += 1
                    try:
                        identity = ActionIdentity.record(scope, groups[rank])
                    except ValueError as error:
                        identities[index] = {"group": groups[rank], "scope": index, "action": scope["role"]}
                        self.finding("missing_native_identity", identities[index], {"reason": str(error)})
                        continue
                    identities[index] = identity
                    key = ActionIdentity.structure_key(identity)
                    occurrences[key].append(index)
                    full_occurrences[ActionIdentity.occurrence_key(identity)].append(index)
                    expected.setdefault(key, (source, index, identity))

                native_reads.update({(groups[rank], query["scope"]): (query["result"], identities[query["scope"]])
                                     for query in raw["state_queries"]})
        if not observed_actions:
            raise ValueError("native case contains no observed actions")

        registrations = {}
        for path in sorted((self.capture / "kv-pools").glob("*.json")):
            registration = json.loads(path.read_text())
            session = registration["registration"]["session"]
            if session in registrations:
                raise ValueError("native session identifies more than one registered pool")
            registrations[session] = registration["parallel"]["rank"]
        connections = Connections()
        histories = {}
        for rank, group in groups.items():
            path = self.capture / "resources" / f"rank-{rank}.json"
            if path.is_file():
                histories[group] = json.loads(path.read_text())
        bindings = BindingComparison(histories, {session: groups[rank] for session, rank in registrations.items()})
        effects = ResourceEffectComparison()
        ordering = ActionOrderComparison()
        for rank, (source, raw, identities) in sources.items():
            group = groups[rank]
            source.observed_pools(raw, registrations, groups)
            actual_dependencies[rank] = CausalGraph.observed(source.graph)
            connections.native(source, raw, rank, ranks[rank], actual_dependencies[rank], identities)
            if group in histories:
                bindings.observed(source, raw, group, identities)
                effects.observed(histories[group], source.graph, group)
            else:
                bindings.findings.append(("binding_evidence", {"group": group}, {"reason": "native resource history is absent"}))
            predicted_dependencies[rank] = CausalGraph()
        sources.clear()

        # Compare contents independently of occurrence pairing, after native remote pools have their actual owners.
        contents = {key: source.snapshot(index, identity["group"]) for key, (source, index, identity) in expected.items()}
        varied = {
            key for key, indices in occurrences.items()
            if any(expected[key][0].snapshot(index, expected[key][2]["group"]) != contents[key] for index in indices[1:])
        }
        compared, matched, matched_dependencies, ended = 0, 0, 0, False
        predicted_counts, full_predicted_counts = Counter(), Counter()
        content_checks, content_matches, paired_counts = Counter(), Counter(), Counter()
        event_ranks, completed, pending = {}, set(), []
        cross_rank_edges = set()
        queries = QueryResults()
        protocol_readiness = protocol_source.inspect()
        protocol = PdPredictionEvidence(protocol_source, protocol_readiness, ingress.origin_ns)
        with self.execution.open() as stream:
            for line in stream:
                row = json.loads(line)
                ingress.observe(row)
                connections.observe(row)
                state_values.observe(row)
                action_results.observe(row)
                query = queries.observe(row)
                protocol.observe(row, query, queries)
                ended = row["kind"] == "end"
                if row["kind"] == "resources":
                    bindings.initialized(row)
                    effects.requests = [request["id"] for request in row["requests"]]
                elif row["kind"] == "notification_declared":
                    rank, = [rank for rank, group in groups.items() if group == row["identity"]["group"]]
                    if row["event"] in event_ranks:
                        raise ValueError("notification reuses an execution event identity")
                    event_ranks[row["event"]] = rank
                    predicted_dependencies[rank].add_events([row["event"]])
                elif row["kind"] == "resource":
                    bindings.released(row)
                    if row["group"] in effects.predicted:
                        effects.predicted[row["group"]].predicted(row, effects.requests)
                elif row["kind"] == "binding":
                    bindings.bound(row)
                if row["kind"] == "edge":
                    if row["relation"] == "communication":
                        connections.communication_edges.add((row["source"], row["target"]))
                    if row["relation"] == "signal":
                        connections.signal_edges.add((row["source"], row["target"]))
                    source_rank, target_rank = event_ranks[row["source"]], event_ranks[row["target"]]
                    if source_rank == target_rank:
                        predicted_dependencies[source_rank].connect(row["source"], row["target"], row["relation"])
                    elif row["relation"] != "communication":
                        cross_rank_edges.add((row["source"], row["target"], row["relation"]))
                elif row["kind"] == "completion":
                    event = row["event"]
                    if event not in event_ranks or event in completed:
                        raise ValueError("completion has an unknown or repeated event identity")
                    completed.add(event)
                    bindings.lifetimes.completed(event)
                if row["kind"] != "graph":
                    continue
                graph = row["graph"]
                root = graph["scopes"][0]
                port = next(port for port in root["ports"] if port["name"] == "call.enter")
                rank = graph["resources"][graph["events"][port["event"]]["resource"]]["rank"]
                if any(resource["rank"] != rank for resource in graph["resources"]):
                    raise ValueError("action contains resources owned by another rank")
                events = row["event_ids"]
                if len(events) != len(graph["events"]) or any(event in event_ranks for event in events):
                    raise ValueError("execution has invalid invocation event identities")
                event_ranks.update(dict.fromkeys(events, rank))
                predicted_dependencies[rank].add_events(events)
                group = ActionIdentity.group(graph, ranks[rank])
                target = GraphContents(graph)
                try:
                    identity = ActionIdentity.record(root, group)
                except ValueError as error:
                    identity = {"group": group, "event": events[port["event"]], "action": root["role"]}
                    self.finding("missing_predicted_identity", identity, {"reason": str(error)})
                key = ActionIdentity.structure_key(identity)
                predicted_counts[key] += 1
                full_predicted_counts[ActionIdentity.occurrence_key(identity)] += 1
                target_key = group, events[port["event"]]

                # Run-local identities retain every writer and occurrence; cross-run pairing happens after multiplicity is known.
                bindings.invocation(graph, identity, events, target_key)
                connections.predicted(target, identity, events, target_key)
                native = expected.get(key)
                if native is None:
                    self.finding("unmatched_prediction", identity, {})
                    continue

                # Equal bodies do not prove when a query ran, what it returned or which communication round it joined.
                snapshot = target.snapshot(0, group)
                content_match = None
                if key not in varied:
                    content_checks[key] += 1
                    content_match = self.compare_contents(identity, contents[key], snapshot)
                    content_matches[key] += content_match
                source, index, _ = native
                pending.append((key, rank, identity, source, index, target_key, ActionBoundaries(target, 0, events),
                                content_match, snapshot if key in varied else None))
        for key, (_, _, identity) in expected.items():
            if not predicted_counts[key]:
                self.finding("unmatched_native", identity, {})
        for key, indices in full_occurrences.items():
            if not full_predicted_counts[key] and predicted_counts[ActionIdentity.structure_key(json.loads(key))]:
                self.finding("unpaired_native_context", json.loads(key), {"observed_calls": len(indices)})
            if len(indices) > 1:
                self.finding("ambiguous_native_identity", json.loads(key), {
                    "observed_calls": len(indices), "predicted_calls": full_predicted_counts[key],
                    "reason": "complete logical context repeats; structure is checked independently",
                })
        for key, count in full_predicted_counts.items():
            if not full_occurrences[key] and ActionIdentity.structure_key(json.loads(key)) in expected:
                self.finding("unpaired_predicted_context", json.loads(key), {"predicted_calls": count})
            if count > 1 and len(full_occurrences[key]) <= 1:
                self.finding("ambiguous_predicted_identity", json.loads(key), {"predicted_calls": count})
        if not ended:
            self.finding("incomplete_execution", {}, {})
        if len(completed) != len(event_ranks):
            self.finding("unfinished_events", {}, {"events": len(event_ranks) - len(completed)})

        # Dynamic waits and state hazards are recorded after graph insertion; compare only after reading the whole execution.
        for graph in predicted_dependencies.values():
            graph.prepare()
        pairs, signatures = {}, {}
        dependency_checks, dependency_matches = Counter(), Counter()
        action_groups = defaultdict(list)
        repeated_groups = defaultdict(dict)
        for key, rank, identity, source, index, target_key, predicted, content_match, snapshot in pending:
            full_key = ActionIdentity.occurrence_key(identity)
            indices = full_occurrences[full_key]
            paired = len(indices) == full_predicted_counts[full_key] == 1
            native = ActionBoundaries(source, index)
            if key not in signatures:
                actual = actual_dependencies[rank].signature(native)
                signatures[key] = actual

                # A common body does not establish common ordering; inspect every observed occurrence separately.
                for repeated_index in occurrences[key][1:]:
                    repeated = actual_dependencies[rank].signature(ActionBoundaries(source, repeated_index))
                    if next(actual.differences(repeated), None) is not None:
                        signatures[key] = None
                        break
            actual = signatures[key]

            # A shared structure is not an occurrence. Pair with its own source scope, never the class representative.
            if paired:
                native = ActionBoundaries(source, indices[0])
                if actual is None:
                    actual = actual_dependencies[rank].signature(native)
                if content_match is None:
                    content_match = self.compare_contents(identity, source.snapshot(indices[0], identity["group"]), snapshot)
                    content_checks[key] += 1
                    content_matches[key] += content_match
            if actual is not None:
                dependency_checks[key] += 1
                estimate = predicted_dependencies[rank].signature(predicted)
                different = False
                for category, detail in actual.differences(estimate):
                    self.finding(category, identity, detail)
                    different = True
                dependency_matches[key] += not different

            # Only occurrence pairs may identify external producers, query results and communication rounds.
            if not paired:
                repeated = repeated_groups[rank]
                if full_key not in repeated:
                    repeated[full_key] = identity, [ActionBoundaries(source, index) for index in indices], []
                repeated[full_key][2].append(predicted)
                continue
            pairs[identity["group"], indices[0]] = target_key
            paired_counts[key] += 1
            action_groups[rank].append((identity, native, predicted))
            compared += 1
            matched += content_match
            matched_dependencies += not different

        for rank in action_groups.keys() | repeated_groups.keys():
            actions = action_groups[rank]
            ordering.compare(actual_dependencies[rank], predicted_dependencies[rank], actions)
            ordering.compare_repeated(actual_dependencies[rank], predicted_dependencies[rank], actions, repeated_groups[rank])

        connections.compare_values(predicted_dependencies, pairs)
        connections.compare_signals(pairs)
        connections.compare_predicted_communication(predicted_dependencies)
        connections.compare_communication(actual_dependencies, pairs)
        bindings.compare(pairs)
        bindings.lifetimes.compare(predicted_dependencies, [
            *cross_rank_edges, *((source, target, "communication") for source, target in connections.communication_edges),
        ])
        for category, identity, detail in (*connections.findings, *bindings.findings, *bindings.lifetimes.findings, *ordering.findings):
            self.finding(category, identity, detail)
        for category, identity, detail in effects.compare():
            self.finding(category, identity, detail)

        for category, identity, detail in (*state_values.compare(protocol_source), *action_results.compare(pairs)):
            self.finding(category, identity, detail)
        for category, identity, detail in (*native_polling.findings, *protocol.polling.findings):
            self.finding(category, identity, detail)

        input_delivery = ingress.compare()
        for category, identity, detail in ingress.findings:
            self.finding(category, identity, detail)
        for instance, result in input_delivery.items():
            if result["status"] != "matched":
                category = "native_evidence" if result["status"] == "missing_native_evidence" else "request_ingress"
                self.finding(category, {"instance": instance}, result)
            if result.get("input_reads", {}).get("status") == "different":
                self.finding("input_read", {"instance": instance}, result["input_reads"])

        recorded_queries = queries.finish()
        checked_cross_rank, unverified_cross_rank = Counter(), Counter()
        for source, target, relation in cross_rank_edges:

            # Notification inputs already passed both actual-edge checks and independent native participant checks.
            covered = relation == "signal" and (source, target) in protocol.notification_edges
            (checked_cross_rank if covered else unverified_cross_rank)[relation] += 1
        query_counts = Counter()
        for key, (result, identity) in native_reads.items():
            if key not in pairs:
                query_counts["unpaired_native"] += 1
                continue
            actual = queries.results[pairs[key]]
            query_counts["compared"] += 1
            if actual != result:
                self.finding("query_outcome", identity, {"native_scope": key[1], "predicted_event": pairs[key][1],
                                                        "native": result, "predicted": actual})
            else:
                query_counts["matched"] += 1
        query_counts["unpaired_predicted"] = len(queries.results.keys() - set(pairs.values()))

        for missing in ("missing_publications", "missing_reservation_updates", "missing_completion_updates", "missing_success_reads"):
            if protocol_readiness[missing]:
                self.finding("native_evidence", {}, {missing: protocol_readiness[missing]})
        repeated = [key for key in expected if len(occurrences[key]) > 1 or predicted_counts[key] > 1]
        input_incomplete = any(result.get("input_reads", {}).get("status") != "matched" for result in input_delivery.values())
        metadata_incomplete = bool(connections.metadata_returns) or any(
            connections.counts.get(f"{origin}_metadata_unobserved", 0) for origin in ("native", "predicted")
        )
        connections.batch_sync.counts["predicted_unobserved_returns"] = sum(
            connections.batch_sync.terminal(part["work"].get("work", {})) for part in connections.action_returns.values()
        )
        batch_sync_incomplete = any(connections.batch_sync.counts.get(f"{origin}_{reason}", 0)
                                    for origin in ("native", "predicted")
                                    for reason in ("unobserved", "unobserved_returns", "inactive_unverified"))
        admission_incomplete = any(check["unobserved"] for checker in (native_polling, protocol.polling)
                                   for check in (checker.operand_counts(), checker.commit_counts()))
        payload_incomplete = any(state_values.counts.get(f"{origin}_unpaired", 0) for origin in ("native", "predicted"))
        connections_incomplete = (not connections.counts["data_evidence_complete"]
                                  or not connections.counts["communication_evidence_complete"]
                                  or connections.counts["data_inputs_unpaired"] or connections.counts["signal_inputs_unpaired"])
        bindings_incomplete = bindings.counts["request_bindings_unpaired"] or bindings.counts["predicted_request_bindings_unpaired"]
        queries_incomplete = query_counts["unpaired_native"] or query_counts["unpaired_predicted"]
        unpaired_calls = {"native": observed_actions - compared, "predicted": sum(predicted_counts.values()) - compared}
        incomplete = (payload_incomplete or any(unpaired_calls.values()) or input_incomplete or metadata_incomplete or admission_incomplete
                      or connections_incomplete or batch_sync_incomplete or bindings_incomplete or queries_incomplete
                      or bool(effects.counts["state_slots"]["unobserved_native"]) or bool(unverified_cross_rank))
        report = {
            **self.outcome(incomplete),
            "native_actions_checked": observed_actions, "predicted_actions_checked": sum(predicted_counts.values()),
            "actions_compared": compared,
            "actions_contents_matched": matched,
            "actions_dependencies_matched": matched_dependencies,
            "unpaired_calls": unpaired_calls,
            "counts": dict(self.counts), "findings": self.findings,
            "connections": connections.counts,
            "batch_synchronization": dict(connections.batch_sync.counts),
            "cross_action_order": ordering.counts,
            "repeated_call_order": {**ordering.repeated_counts,
                                    "scope": "Sets of rank-local causal positions relative to uniquely paired call entries/returns. Counts, payloads, effects, cross-rank rounds and timing are not paired; no source occurrence is selected by matching its position."},
            "physical_bindings": bindings.counts,
            "kv_lifetimes": bindings.lifetimes.report(),
            "resource_effects": effects.counts,
            "state_values": state_values.counts,
            "action_results": dict(action_results.counts),
            "query_results_recorded": recorded_queries,
            "completion_reads_checked": queries.completion_reads,
            "completion_notifications_checked": queries.notifications,
            "pd_read_prerequisites": protocol.finish(),
            "native_protocol_readiness": protocol_readiness,
            "native_polling_operands": native_polling.operand_counts(),
            "native_admission_decisions": native_polling.commit_counts(),
            "query_outcomes": dict(query_counts),
            "request_ingress": input_delivery,
            "repeated_calls": [{"work": expected[key][2], "classification": "structural_class", "paired_calls": paired_counts[key],
                                "observed_calls": len(occurrences[key]), "predicted_calls": predicted_counts[key],
                                "source_contents_uniform": key not in varied,
                                "contents_checked": content_checks[key], "contents_matched": content_matches[key],
                                "source_dependencies_uniform": signatures[key] is not None if key in signatures else None,
                                "dependencies_checked": dependency_checks[key], "dependencies_matched": dependency_matches[key]}
                               for key in repeated],
            "cross_rank_dependencies": {"protocol_checked_edges": dict(checked_cross_rank), "unverified_edges": dict(unverified_cross_rank)},
            "not_checked": ["unpaired query outcomes and exact PD notification arrival times", "unpaired external input reads", "unnamed internal event topology",
                            "cross-rank causality outside verified communication and notification protocols",
                            "intra-call resource effect timing and in-flight resource lifetimes", "request-slot bindings during native transport",
                            "per-occurrence pairing, placement and timing of calls whose full logical context repeats", "PD unused padding and unsupported multimodal/logprob/speculative values; controller output values outside recorded progress and publication fields"],
            "scope": "Every invocation retains its run-local identity; only one-to-one full logical matches, including ancestor contexts, enter cross-run pairing. Structural classes omit ancestor state only for common-content checks; they never identify external producers or causal boundaries. Contents and within-action boundary reachability/work placement are checked separately for every predicted occurrence when their respective observed signatures are uniform. This does not pair external producers, effects, query outcomes or communication rounds. All predicted communication edges are separately checked against run-local channel participants and causal order, including repeated calls; this does not establish cross-run occurrence pairing or payload correctness. A unique full-context pair uses its own observed scope even when the structural class varies; unpaired variation remains unverified. No signature is selected by resemblance to the prediction. Cross-action order, data writers/export readiness and native protocol participant dependencies retain their independent checks. Paths include the full rank-local graph and dynamic prediction edges, including unnamed intermediates. Event numbers, raw edge counts and measured times are not matched. Differences require diagnosis, not automatic attribution to core bugs.",
        }
        return self.publish(report)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", type=Path, required=True)
    parser.add_argument("--graphs", type=Path, required=True)
    parser.add_argument("--execution", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--details", action="store_true", help="retain every observed difference")
    arguments = parser.parse_args()
    raise SystemExit(ActionComparison(arguments.capture, arguments.graphs, arguments.execution, arguments.output, arguments.details).run())


if __name__ == "__main__":
    main()
