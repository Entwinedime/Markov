"""Check each run's producer readiness and compare native value provenance and communication peers."""

from collections import defaultdict

from actions import ActionIdentity
from batch_sync import BatchSynchronization
from protocol_reads import PdPollingEvidence


class Connections:
    def __init__(self):
        self.values, self.reads, self.published = {}, {}, {}
        self.ports, self.ready, self.complete = {}, {}, {}
        self.channels = defaultdict(lambda: defaultdict(list))
        self.predicted_channels = defaultdict(lambda: defaultdict(list))
        self.communication_edges = set()
        self.native_communication_edges = set()
        self.metadata_returns = {}
        self.endpoints = {}
        self.action_returns = {}
        self.batch_sync = BatchSynchronization(self.finding)
        self.signal_inputs, self.signal_reads, self.signal_outputs = {}, {}, {}
        self.signal_events, self.signal_edges = {}, set()
        self.query_consumers = {}
        self.findings = []
        self.counts = {}
        self.value_evidence_complete = True

    def finding(self, category, identity, **detail):
        self.findings.append((category, identity, detail))

    def native(self, contents, raw, rank, instance, dependencies, identities):
        graph, owners = contents.graph, {}
        endpoints = []
        metadata = {}
        for record in raw.get("action_values", ()):
            if "collective" not in record["values"]:
                continue
            index = record["scope"]
            if type(index) is not int or not 0 < index < len(raw["scopes"]) or raw["scopes"][index]["role"] != "metadata_collective":
                raise ValueError("metadata evidence requires a unique observed collective")
            index = contents.source_ids[index]
            if index in metadata:
                raise ValueError("metadata collective has competing native observations")
            metadata[index] = record["values"]["collective"]
        for root, scope in enumerate(graph["scopes"]):
            if not ActionIdentity.selected(root, scope):
                continue
            identity = identities[root]
            key = (identity["group"], root)
            for index, path in contents.paths(root).items():
                if index in owners:
                    raise ValueError("native scope belongs to more than one action")
                owners[index] = key, path, identity

        # Native handles or declared scope boundaries identify producers independently of projected signal names.
        signals, inputs = defaultdict(set), []
        for index, (key, path, identity) in owners.items():
            projected = {(port["name"], port["direction"]): port for port in graph["scopes"][index]["ports"]}
            origins = graph["scopes"][index]["bindings"]["source_scopes"]
            native_ports = (port for origin in origins for port in raw["scopes"][origin]["ports"])
            for port in native_ports:
                if port["use"] != "signal":
                    continue
                label = key, path, port["direction"], port["name"]
                if "boundary" in port:
                    reference = port["boundary"]
                    generation = ("boundary", reference["scope"], reference["interval"], reference["side"])
                else:
                    generation = ("point", port["point"]) if "point" in port else ("producer", port["producer"])
                boundary = projected[port["name"], port["direction"]]
                if boundary["use"] != "signal":
                    raise ValueError("native completion source lost its signal port")
                if port["direction"] == "output":
                    signals[generation].add(label)
                else:
                    inputs.append((label, generation, identity))

        # Queries observe a generation without waiting for it; the native record point still identifies its producer.
        for query in raw["state_queries"]:
            if query["kind"] != "event":
                continue
            key, path, identity = owners[contents.source_ids[query["scope"]]]
            inputs.append(((key, path, "query", "completion"), ("point", query["point"]), identity))
        for label, generation, identity in inputs:
            producers = signals[generation]
            if not producers:
                self.finding("native_evidence", identity, scope=label[1], port=label[3],
                             reason="completion handle has no observed producer")
            self.signal_inputs[label] = producers, identity

        # The writing child and the scope exporting its value need not be the same scope.
        exports = defaultdict(list)
        for index, (key, path, _) in owners.items():
            for port in graph["scopes"][index]["ports"]:
                if port["use"] != "data" or port["direction"] != "output":
                    continue
                tensor = port["tensor"]
                writer = owners.get(tensor["writer"])
                if writer is not None and writer[0] == key:
                    exports[tensor["writer"], tensor["storage"], tensor["version"]].append(
                        ((key, path, "output", port["name"]), port["event"]))

        for index, (key, path, identity) in owners.items():
            scope = graph["scopes"][index]
            for port in scope["ports"]:
                if port["use"] != "data" or port["direction"] != "input" or 0 in port["tensor"]["shape"]:
                    continue
                tensor = port["tensor"]
                writer = owners.get(tensor["writer"])
                if writer is None:
                    self.finding("native_evidence", identity, scope=path, port=port["name"], reason="data writer has no observed action")
                    self.value_evidence_complete = False
                    continue
                if writer[0] == key:
                    continue

                outputs = [label for label, event in exports[tensor["writer"], tensor["storage"], tensor["version"]]
                           if dependencies.reaches(event, port["event"])]
                if not outputs:
                    self.finding("native_evidence", identity, scope=path, port=port["name"], reason="writer action has no connected export for this value")
                    self.value_evidence_complete = False
                    continue
                consumer = key, path, "input", port["name"]
                self.values[consumer] = (writer[:2], outputs, rank, identity)

            if communication := scope.get("communication"):
                entered = next(port["event"] for port in scope["ports"] if port["name"] == "call.enter")
                completion = next(port for port in scope["ports"] if "rendezvous" in port)
                endpoint = {
                    **communication, "entered": entered, "scope": (key, path), "work": identity,
                    "ready": completion["rendezvous"], "complete": completion["event"],
                    "metadata": scope["role"] == "metadata_collective", "values": metadata.get(index),
                    "elements": scope["parameters"].get("elements"), "group_role": scope["bindings"].get("collective_group"),
                }
                self.channels[instance, communication["channel"]][rank].append(endpoint)
                endpoints.append(endpoint)

        observed = set()
        for record in raw.get("action_values", ()):
            if "batch_sync" not in record["values"]:
                continue
            parts = record["values"]["segments"]
            if (not parts or any(type(part) is not int or not 0 < part < len(raw["scopes"]) for part in parts)
                    or parts != sorted(set(parts)) or record["scope"] != parts[-1] or contents.source_ids[parts[-1]] in observed):
                raise ValueError("batch synchronization requires its actual ordered segment span")
            parent = raw["scopes"][parts[0]]["parent"]
            if parts != [i for i, scope in enumerate(raw["scopes"]) if scope["parent"] == parent and scope["role"] == "segment"]:
                raise ValueError("batch synchronization observation omits or borrows an invocation segment")
            parts = [contents.source_ids[part] for part in parts]
            self.batch_sync.span([identities[part] for part in parts])
            identity = identities[parts[-1]]
            candidates = [endpoint for endpoint in endpoints if parts[0] <= endpoint["scope"][0][1] <= parts[-1]]
            self.batch_sync.attach(record["values"]["batch_sync"], candidates, identity, "native")
            observed.add(parts[-1])
        expected = {scope for scope, identity in identities.items() if self.batch_sync.terminal(identity.get("work", {}))}
        self.batch_sync.counts["native_unobserved_returns"] += len(expected - observed)

    def predicted(self, contents, identity, events, key):
        graph, paths = contents.graph, contents.paths(0)
        group = identity["group"]
        continuation = identity.get("work", {}).get("continuation") or {}
        if continuation.get("owner") == "batch_sync_exchange":
            returned = next(port["event"] for port in graph["scopes"][0]["ports"] if port["name"] == "call.return")
            self.action_returns[events[returned]] = {"key": key, "work": identity, "completed_ns": None}
        for index, path in paths.items():
            scope = graph["scopes"][index]
            if scope["role"] == "query" and scope["work"]["context"]["query"] == "event":
                returned = next(port["event"] for port in scope["ports"] if port["name"] == "call.return")
                self.query_consumers[events[returned]] = (key, path, "query", "completion"), identity
            if communication := scope.get("communication"):
                entered = next(port["event"] for port in scope["ports"] if port["name"] == "call.enter")
                rank = graph["resources"][graph["events"][entered]["resource"]]["rank"]
                if communication["rank"] != rank:
                    raise ValueError("communication endpoint belongs to another worker")
                endpoint = {
                    **communication, "entered": events[entered], "scope": (key, path), "work": identity,
                    "metadata": scope["role"] == "metadata_collective", "values": None,
                    "elements": scope["parameters"].get("elements"), "group_role": scope["bindings"].get("collective_group"),
                }
                self.endpoints[endpoint["scope"]] = endpoint
                self.predicted_channels[communication["channel"]][rank].append(endpoint)
                if endpoint["metadata"]:
                    returned = next(port["event"] for port in scope["ports"] if port["name"] == "call.return")
                    self.metadata_returns[events[returned]] = {"endpoint": endpoint, "completed_ns": None}
            for port in scope["ports"]:
                if port["use"] == "signal":
                    label = key, path, port["direction"], port["name"]
                    self.signal_events[label] = events[port["event"]]
                    if port["direction"] == "output":
                        if port["signal"] in self.signal_outputs:
                            raise ValueError("predicted completion generation was published twice")
                        self.signal_outputs[port["signal"]] = label
                    else:
                        self.signal_reads[label] = self.signal_outputs.get(port["signal"]), identity
                if "rendezvous" in port:
                    self.ready[key, path] = events[port["rendezvous"]]
                    self.complete[key, path] = events[port["event"]]
                if port["use"] != "data":
                    continue
                label = key, path, port["direction"], port["name"]
                self.ports[label] = events[port["event"]]
                tensor = port["tensor"]
                if port["direction"] != "input" or 0 in tensor["shape"]:
                    continue
                writer = tensor["writer"]
                if writer is not None:
                    producer, output = (key, paths[writer]), None
                else:
                    producer, output = self.published.get((group, tensor["storage"], tensor["version"]), (None, None))
                self.reads[label] = producer, output, identity

        # Only root exports cross an invocation boundary; internal or pass-through ports cannot replace the producer.
        for port in graph["scopes"][0]["ports"]:
            if port["use"] != "data" or port["direction"] != "output":
                continue
            tensor = port["tensor"]
            if tensor["writer"] is not None:
                self.published[group, tensor["storage"], tensor["version"]] = (
                    (key, paths[tensor["writer"]]), (key, paths[0], "output", port["name"]),
                )

    def observe(self, record):
        if record["kind"] == "completion" and record["event"] in self.action_returns:
            self.action_returns[record["event"]]["completed_ns"] = record["time_ns"]
        if record["kind"] == "action_values" and "batch_sync" in record["values"]:
            parts = record["values"]["segments"]
            if not parts or len(parts) != len(set(parts)) or record["boundary"] != parts[-1]:
                raise ValueError("batch synchronization requires its own completed segments")
            span = [self.action_returns[part] for part in parts]
            if any(part["work"]["group"] != record["group"] or part["completed_ns"] is None
                   or record["time_ns"] < part["completed_ns"] for part in span):
                raise ValueError("batch synchronization result precedes its invocation return")
            self.batch_sync.span([part["work"] for part in span])
            keys = [part["key"][1] for part in span]
            if keys != sorted(set(keys)):
                raise ValueError("batch synchronization segments are out of invocation order")
            first, last = keys[0], keys[-1]
            expected = {event for event, part in self.action_returns.items()
                        if part["work"]["group"] == record["group"] and first <= part["key"][1] <= last
                        and (part["work"].get("work", {}).get("continuation") or {}).get("owner") == "batch_sync_exchange"}
            if set(parts) != expected:
                raise ValueError("batch synchronization observation omits an invocation segment")
            candidates = [endpoint for (key, _), endpoint in self.endpoints.items()
                          if key[0] == record["group"] and first <= key[1] <= last]
            self.batch_sync.attach(record["values"]["batch_sync"], candidates, span[-1]["work"], "predicted")
            for part in parts:
                del self.action_returns[part]
        if record["kind"] == "completion_read" and record["boundary"] in self.query_consumers:
            consumer, identity = self.query_consumers.pop(record["boundary"])
            self.signal_reads[consumer] = self.signal_outputs[record["signal"]], identity
        elif record["kind"] == "completion" and record["event"] in self.metadata_returns:
            self.metadata_returns[record["event"]]["completed_ns"] = record["time_ns"]
        elif record["kind"] == "action_values" and "collective" in record["values"]:
            pending = self.metadata_returns.pop(record["boundary"])
            endpoint = pending["endpoint"]
            if (record["group"] != endpoint["work"]["group"] or pending["completed_ns"] is None
                    or record["time_ns"] < pending["completed_ns"]):
                raise ValueError("metadata result precedes its own completed native call")
            endpoint["values"] = record["values"]["collective"]

    def check_metadata(self, endpoints, origin):
        self.batch_sync.check(endpoints, origin)
        if not any(endpoint["metadata"] for endpoint in endpoints):
            return
        if not all(endpoint["metadata"] for endpoint in endpoints):
            self.finding("metadata_protocol", endpoints[0]["work"], reason="participants disagree on metadata ownership")
            return

        # A position-wise polling MIN must refer to the same ordered requests on every peer.
        queues = [PdPollingEvidence.request_order(endpoint["work"]["work"])
                  if PdPollingEvidence.in_poll(endpoint["work"]["work"]) else None for endpoint in endpoints]
        if any(queue != queues[0] for queue in queues):
            self.finding("metadata_protocol", endpoints[0]["work"], origin=origin,
                         reason="PD polling operands refer to different peer request queues")
            return
        field = f"{origin}_metadata_exchanges"
        self.counts[field] = self.counts.get(field, 0) + 1
        if any(endpoint["values"] is None for endpoint in endpoints):
            field = f"{origin}_metadata_unobserved"
            self.counts[field] = self.counts.get(field, 0) + 1
            return

        collective = endpoints[0]["work"]["work"]["context"]["collective"]
        root = collective["root"]
        if root is not None and (type(root) is not int or not 0 <= root < len(endpoints)):
            raise ValueError("metadata broadcast root is outside its native group")
        for position, endpoint in enumerate(endpoints):
            values = endpoint["values"]
            size = endpoint["elements"] if root is None or position == root else 0
            if (endpoint["work"]["work"]["context"]["collective"] != collective
                    or not isinstance(values["input"], list) or not isinstance(values["output"], list)
                    or endpoint["elements"] != endpoints[0]["elements"] or len(values["input"]) != size
                    or any(type(value) not in (bool, int, float) for row in values.values() for value in row)):
                raise ValueError("metadata values disagree with their declared scalar collective")
        operands = [endpoint["values"]["input"] for endpoint in endpoints]
        operation = collective["operation"]
        if operation == "broadcast":
            expected = operands[collective["root"]]
        elif operation == "all_gather_into_tensor":
            expected = [value for operand in operands for value in operand]
        elif operation == "all_reduce" and collective["reduction"] == "RedOpType.MIN":
            expected = [min(values) for values in zip(*operands, strict=True)]
        else:
            raise ValueError("metadata evidence uses an unsupported scalar collective")

        # Compute from each run's actual operands; no core/broker reduction or cross-run polling index is reused.
        for endpoint in endpoints:
            if endpoint["values"]["output"] != expected:
                self.finding("metadata_values", endpoint["work"], expected=expected, actual=endpoint["values"]["output"], origin=origin)
                return
        field = f"{origin}_metadata_matched"
        self.counts[field] = self.counts.get(field, 0) + 1

    @staticmethod
    def mapped(label, pairs):
        key = pairs.get(label[0])
        return None if key is None else (key, *label[1:])

    def check_predicted_readiness(self, dependencies, event_ranks):
        checked, ready = 0, 0
        for consumer, (writer, output, identity) in self.reads.items():
            if writer is not None and writer[0] == consumer[0]:
                continue
            checked += 1
            if output is None:
                self.finding("data_producer", identity, scope=consumer[1], port=consumer[3],
                             origin="predicted", reason="external data input has no published storage/version producer")
                continue
            source, target = self.ports[output], self.ports[consumer]
            if not dependencies.reaches((event_ranks[source], source), (event_ranks[target], target)):
                self.finding("data_readiness", identity, scope=consumer[1], port=consumer[3], origin="predicted",
                             producer=output, reason="external data input is not gated by its own producer export")
            else:
                ready += 1
        self.counts.update(predicted_data_inputs_checked=checked, predicted_data_inputs_ready=ready)

        checked, ready = 0, 0
        for consumer, (producer, identity) in self.signal_reads.items():

            # A query observes completion; only an explicit wait requires a dependency on that generation.
            if consumer[2] == "query":
                continue
            checked += 1
            if producer is None:
                self.finding("signal_producer", identity, scope=consumer[1], port=consumer[3],
                             origin="predicted", reason="completion wait has no published generation")
            elif (self.signal_events[producer], self.signal_events[consumer]) not in self.signal_edges:
                self.finding("signal_dependency", identity, scope=consumer[1], port=consumer[3],
                             origin="predicted", reason="wait has no direct dependency on its bound completion event")
            else:
                ready += 1
        self.counts.update(predicted_signal_inputs_checked=checked, predicted_signal_inputs_ready=ready)

    def compare_signals(self, pairs):
        matched, unpaired = 0, 0
        targets = set(pairs.values())
        for native_consumer, (native_producers, identity) in self.signal_inputs.items():
            if not native_producers:
                continue
            consumer = self.mapped(native_consumer, pairs)
            producers = {self.mapped(producer, pairs) for producer in native_producers}
            read = self.signal_reads.get(consumer)
            producer = read[0] if read is not None else None
            if consumer is None or None in producers or (producer is not None and producer[0] not in targets):
                unpaired += 1
                continue
            if producer not in producers:
                self.finding("signal_producer", identity, scope=consumer[1], port=consumer[3],
                             expected_producers=sorted(producers), predicted_producer=producer)
            else:
                matched += 1
        self.counts.update(signal_inputs=len(self.signal_inputs), signal_inputs_matched=matched, signal_inputs_unpaired=unpaired)

    def compare_values(self, dependencies, pairs):
        matched, unpaired = 0, 0
        targets = set(pairs.values())
        for native_consumer, (native_writer, native_outputs, rank, identity) in self.values.items():
            consumer, writer = self.mapped(native_consumer, pairs), self.mapped(native_writer, pairs)
            outputs = [self.mapped(output, pairs) for output in native_outputs]
            read = self.reads.get(consumer)
            predicted_writer = read[0] if read is not None else None
            if (consumer is None or writer is None or None in outputs
                    or (predicted_writer is not None and predicted_writer[0] not in targets)):
                unpaired += 1
                continue
            if predicted_writer != writer:
                self.finding("data_producer", identity, scope=consumer[1], port=consumer[3],
                             expected_writer=writer, predicted_writer=predicted_writer)
            elif consumer not in self.ports or any(output not in self.ports for output in outputs):
                self.finding("data_readiness", identity, scope=consumer[1], port=consumer[3], reason="mapped data boundary is absent")
            elif any(not dependencies.reaches((rank, self.ports[output]), (rank, self.ports[consumer])) for output in outputs):
                self.finding("data_readiness", identity, scope=consumer[1], port=consumer[3], reason="consumer is not gated by its producer exports")
            else:
                matched += 1
        self.counts.update(data_inputs=len(self.values), data_inputs_matched=matched, data_inputs_unpaired=unpaired,
                           data_evidence_complete=self.value_evidence_complete)

    def edge_details(self, edges):
        result = []
        for source, target in sorted(edges)[:3]:
            record = {}
            for name, event, ports in (("source", source, self.ready), ("target", target, self.complete)):
                record[name] = {"event": event, "ports": [{"invocation": {"group": key[0], "event": key[1]}, "scope": path}
                                                          for (key, path), value in ports.items() if value == event]}
            result.append(record)
        return result

    def exchanges(self, channels, dependencies, category):
        for channel, ranks in channels.items():
            first = next(iter(ranks.values()))[0]
            members = first["members"]
            if set(ranks) != set(members) or len({len(values) for values in ranks.values()}) != 1:
                self.finding(category, first["work"], channel=channel, reason="communication participants or call counts are incomplete")
                yield None
                continue
            ordered = []
            valid = True
            for rank in members:
                calls = sorted(ranks[rank], key=lambda value: (dependencies[rank].positions[value["entered"]], value["order"]))

                # Protocol order is run-local: never align separate executions by a polling ordinal.
                if any(not dependencies[rank].reaches(left["entered"], right["entered"]) for left, right in zip(calls, calls[1:])):
                    self.finding(category, first["work"], channel=channel, reason="calls have no established protocol order")
                    valid = False
                ordered.append(calls)
            if not valid:
                yield None
                continue

            for endpoints in zip(*ordered, strict=True):
                if any(value["members"] != members or value["operation"] != endpoints[0]["operation"]
                       or not set(value["required"]) <= set(members) for value in endpoints):
                    self.finding(category, first["work"], channel=channel, reason="peers disagree on operation or members")
                    yield None
                else:
                    yield endpoints

    def compare_predicted_communication(self, dependencies):
        expected, exchanges, unresolved = set(), 0, False
        for endpoints in self.exchanges(self.predicted_channels, dependencies, "communication_protocol"):
            if endpoints is None:
                unresolved = True
                continue
            exchanges += 1
            self.check_metadata(endpoints, "predicted")
            peers = {endpoint["rank"]: endpoint for endpoint in endpoints}
            for endpoint in endpoints:
                destination = self.complete.get(endpoint["scope"])
                for rank in endpoint["required"]:
                    source = self.ready.get(peers[rank]["scope"])
                    if source is None or destination is None:
                        self.finding("communication_endpoint", endpoint["work"], scope=endpoint["scope"][1],
                                     reason="declared communication boundary is absent")
                        unresolved = True
                    else:
                        expected.add((source, destination))

        # Native ambiguity must not hide missing or spurious edges in the target's own protocol.
        missing, extra = expected - self.communication_edges, self.communication_edges - expected
        for name, edges in (("missing", missing), ("extra", extra)):
            if edges and (name == "missing" or not unresolved):
                self.finding("communication_protocol", {}, **{name: len(edges)}, examples=self.edge_details(edges))
        self.counts.update(predicted_communication_exchanges=exchanges,
                           predicted_communication_edges_expected=len(expected),
                           predicted_communication_edges_matched=len(expected & self.communication_edges),
                           predicted_communication_protocol_complete=not (unresolved or missing or extra))

    def native_communication(self, dependencies):
        self.native_rounds = list(self.exchanges(self.channels, dependencies, "native_evidence"))
        complete = True
        exchanges = 0
        for endpoints in self.native_rounds:
            if endpoints is None:
                complete = False
                continue
            exchanges += 1
            self.check_metadata(endpoints, "native")
            peers = {endpoint["rank"]: endpoint for endpoint in endpoints}
            for endpoint in endpoints:
                for rank in endpoint["required"]:
                    self.native_communication_edges.add(((rank, peers[rank]["ready"]), (endpoint["rank"], endpoint["complete"])))
        self.counts.update(communication_channels=len(self.channels), communication_exchanges=exchanges,
                           native_communication_protocol_complete=complete)

    def compare_communication(self, pairs):
        expected, unresolved, unpaired = set(), False, 0
        for endpoints in self.native_rounds:
            if endpoints is None:
                unresolved = True
                continue
            peers = {endpoint["rank"]: endpoint for endpoint in endpoints}
            for endpoint in endpoints:
                destination_scope = self.mapped(endpoint["scope"], pairs)
                destination = self.complete.get(destination_scope)
                if endpoint["metadata"] and endpoint["values"] is not None and destination_scope in self.endpoints:
                    target = self.endpoints[destination_scope]
                    if target["values"] is not None:
                        field = "metadata_operands_compared"
                        self.counts[field] = self.counts.get(field, 0) + 1
                        if endpoint["values"] != target["values"]:
                            self.finding("metadata_operands", endpoint["work"], native=endpoint["values"], predicted=target["values"])
                if "batch_sync" in endpoint and destination_scope in self.endpoints:
                    target = self.endpoints[destination_scope]
                    if "batch_sync" in target:
                        self.batch_sync.counts["endpoints_compared"] += 1
                        if endpoint["batch_sync"] != target["batch_sync"]:
                            self.finding("batch_sync_operands", endpoint["work"], native=endpoint["batch_sync"], predicted=target["batch_sync"])
                for rank in endpoint["required"]:
                    source_scope = self.mapped(peers[rank]["scope"], pairs)
                    if source_scope is None or destination_scope is None:
                        unpaired += 1
                        unresolved = True
                        continue
                    source = self.ready.get(source_scope)
                    if source is None or destination is None:
                        self.finding("communication_endpoint", endpoint["work"], scope=endpoint["scope"][1], reason="mapped communication boundary is absent")
                        unresolved = True
                    else:
                        expected.add((source, destination))

        missing, extra = expected - self.communication_edges, self.communication_edges - expected
        if missing:
            self.finding("communication_dependency", {}, missing=len(missing), examples=self.edge_details(missing))

        # Incomplete native evidence cannot label unaccounted prediction edges as spurious.
        if extra and not unresolved:
            self.finding("communication_dependency", {}, extra=len(extra), examples=self.edge_details(extra))
        self.counts.update(communication_edges_expected=len(expected), communication_edges_observed=len(self.communication_edges),
                           communication_edges_matched=len(expected & self.communication_edges), communication_edges_unpaired=unpaired,
                           communication_evidence_complete=not unresolved)
