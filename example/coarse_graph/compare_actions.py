"""Compare action contents, causal boundaries and data/communication connections with a native case."""

import argparse
import json
from collections import Counter, defaultdict
from pathlib import Path

from actions import ActionIdentity, QueryResults
from contents import GraphContents
from execution_times import ExecutionTimes
from execution_inputs import ConsumedInputs
from stream_queues import StreamSynchronization
from native_case import NativeCase
from ingress import IngressEvidence
from dependencies import ActionBoundaries, ActionOrderComparison, CausalGraph
from protocol_reads import PdPollingEvidence, PdPredictionEvidence
from state_values import ActionResults, StateValueComparison


class ActionComparison:
    evidence_categories = NativeCase.evidence_categories | frozenset({
        "state_binding_evidence", "ordering_evidence", "input_read_evidence", "stream_evidence",
        "missing_predicted_identity",
        "ambiguous_native_identity", "ambiguous_predicted_identity",
        "unpaired_native_context", "unpaired_predicted_context",
        "incomplete_execution", "unfinished_events",
        "native_tail_unexecuted",
    })

    def __init__(self, capture, graphs, execution, output, details):
        self.capture, self.graphs, self.execution, self.output, self.details = capture, graphs, execution, output, details
        self.counts, self.findings = Counter(), []

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
        for category, detail in GraphContents.differences(actual, predicted, self.details):
            self.finding(category, identity, detail)
            different = True
        return not different

    def publish(self, report):
        self.output.mkdir(parents=True)
        (self.output / "actions.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")

        checks, state, counts = report["action_checks"], report["state_lifetimes"], report["counts"]
        connections = report["connections"]
        reads = report["resource_effects"]["reads"]
        streams = report["stream_synchronization"]
        order = report["action_order"]
        outside_flow = sum(value["input_reads"]["outside_flow"] for value in report["request_ingress"].values() if "input_reads" in value)
        rows = [
            "# 执行结构独立对照", "",
            f"整例结果：`{report['status']}`；证据覆盖：`{report['coverage']}`。", "",
            "以下分别说明实际证明了什么。局部对应不能替代整例通过，轨迹差异数也不是独立 Bug 数。", "",
            "| 检查对象 | 已检查 | 结果与未覆盖范围 |", "| --- | --- | --- |",
        ]
        for name, label in (("contents", "模板展开内容"), ("dependencies", "调用内具名边界依赖")):
            value = checks[name]
            rows.append(f"| {label} | {value['checked']} / {checks['predicted_occurrences']} 次预测调用 | "
                        f"对应 {value['matched']}，差异 {value['checked'] - value['matched']}，未验证 "
                        f"{checks['predicted_occurrences'] - value['checked']}；共享内容检查不建立逐次调用配对 |")

        rows.extend([
            f"| 运行内生产与消费 | {connections['predicted_data_inputs_checked']} 个非空外部数据输入、"
            f"{connections['predicted_signal_inputs_checked']} 个完成等待 | 数据就绪保护 "
            f"{connections['predicted_data_inputs_ready']} 项、完成等待保护 {connections['predicted_signal_inputs_ready']} 项；"
            "不依赖跨运行调用配对，查询不当作等待 |",
            f"| 动态stream等待 | {streams.get('waits_checked', 0)} / {streams.get('declared_waits', 0)} 次等待 | "
            f"{streams['matched_dependencies']} / {streams['required_dependencies']} 条前沿依赖对应，"
            f"额外依赖 {streams['extra_dependencies']}，缺失激活事实 {streams['activations_missing']}；按实际接收时刻取队列快照 |",
            f"| 运行内物理状态 | {state.get('tensor_ports_checked', 0)} 个非空可见张量端口、"
            f"{state.get('kv_regions_checked', 0)} 个 KV 逻辑区间、"
            f"{state.get('field_bindings_checked', 0)} 条字段绑定 | 未完整绑定的 KV 区间 "
            f"{state.get('kv_regions_unbound', 0)}、字段访问 {state.get('field_bindings_missing', 0)}；"
            f"{state.get('pending_conflicts_ordered', 0)} / {state.get('pending_conflicts', 0)} 个在途冲突有保护路径 |",
            f"| 跨运行逐次对应 | {report['actions_compared']} 个已建立对应的调用 | 内容对应 "
            f"{report['actions_contents_matched']}，内部依赖对应 {report['actions_dependencies_matched']}；"
            f"其余原生 / 预测调用未配对 {report['unpaired_calls']['native']} / {report['unpaired_calls']['predicted']} |",
            f"| 执行轨迹 | 两次运行的最小次序关系和资源历史 | 原生关系在预测中不成立 "
            f"{order.get('missing_relations', 0)} 条，预测关系在原生中不成立 {order.get('extra_relations', 0)} 条；"
            f"事件别名差异 {order.get('alias_conflicts', 0)}，重复位置差异 {counts.get('repeated_call_order', 0)}，"
            f"资源历史差异 {counts.get('resource_effects', 0)}；不是独立 Bug 数 |",
            f"| 资源条件读取 | 已记录：原生 {sum(value['count'] for value in reads['native'])} 次、"
            f"预测 {sum(value['count'] for value in reads['predicted'])} 次标量查询 | "
            "原始 getter 与语义切口已核对；次数和值域不作为跨运行逐次对应证明 |",
            "", "控制交互还需核对输入交付、查询、通知、准入及决定性时间条件；内容或状态安全通过不能代替它们。",
            f"输入读取按实际serving活动范围对照；范围外{outside_flow}次空读取仍保留于执行记录，全部非空请求交付继续检查。",
            "未配对、协议缺失和未触发的复用继续保留证据限制；零在途页归还不证明在途复用正确。",
            "完整分类、代表问题与各检查边界保留在 actions.json。", "",
        ])
        (self.output / "actions.md").write_text("\n".join(rows))
        return 0 if report["status"] == "actions_matched" else 1

    def run(self):
        capture = json.loads((self.capture / "capture.json").read_text())
        native_case = NativeCase(self.capture, capture, self.graphs)
        groups, ranks = native_case.groups, native_case.ranks
        sources = native_case.sources
        cpu_protocol, protocol_source = native_case.cpu, native_case.protocol
        expected, predicted_dependencies = {}, {}
        occurrences, full_occurrences = defaultdict(list), defaultdict(list)
        native_classes, predicted_classes = defaultdict(set), defaultdict(set)
        native_scope_keys, native_reads = {}, {}
        native_polling = PdPollingEvidence(protocol_source.admission_members)
        state_values = StateValueComparison(groups)
        action_results = ActionResults(groups)
        ingress = IngressEvidence(self.capture, capture["instances"])
        for instance in capture["instances"]:
            for rank in instance["observed_roles"]:
                graph = json.loads((self.graphs / f"{rank}-graph.json").read_text())
                raw = json.loads((self.capture / f"rank-{rank}.json").read_text())
                source = native_case.observe(rank, graph, raw)
                identities = sources[rank][2]
                native_polling.native(raw, groups[rank])
                state_values.source(raw, groups[rank])
                action_results.source(raw, groups[rank], instance["mode"], source.source_ids)
                ingress.native(instance["id"], raw)
                for query in raw["state_queries"]:
                    if query["kind"] == "clock":
                        continue
                    index = source.source_ids[query["scope"]]
                    native_reads[groups[rank], index] = query["result"], identities[index]
        native_case.finish()
        observed_actions, controls = native_case.observed_actions, native_case.controls
        for category, identity, detail in native_case.findings:
            self.finding(category, identity, detail)

        # Use the native ABI only when every declaration is host-only; predicted contents never select a source class.
        for rank, (source, _, identities) in sources.items():
            for index, identity in identities.items():
                if "work" not in identity:
                    continue
                key = source.structure_key(index, identity, controls)
                full_key = ActionIdentity.logical_key(identity)
                occurrences[key].append(index)
                full_occurrences[full_key].append(index)
                native_classes[full_key].add(key)
                native_scope_keys[rank, index] = key
                expected.setdefault(key, (source, index, identity))

        connections, bindings = native_case.connections, native_case.bindings
        effects = native_case.effects
        ordering = ActionOrderComparison(self.details)
        predicted_dependencies = {rank: CausalGraph() for rank in sources}
        native_evidence = native_case.report(self.details)
        condition_inputs = ConsumedInputs(native_case, ingress)
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
        execution_times = ExecutionTimes()
        stream_synchronization = StreamSynchronization()
        protocol_readiness = native_case.readiness
        protocol = PdPredictionEvidence(protocol_source, protocol_readiness, ingress.origin_ns)
        with self.execution.open() as stream:
            for line in stream:
                row = json.loads(line)
                condition_inputs.observe(row)
                execution_times.observe(row)
                stream_synchronization.observe(row)
                cpu_protocol.observe(row)
                cpu_service = row["kind"] == "graph" and row["graph"]["scopes"][0]["role"] == "cpu_service"
                if not cpu_service:
                    ingress.observe(row)
                    effects.observe(row)
                    connections.observe(row)
                    state_values.observe(row)
                    action_results.observe(row)
                    query = queries.observe(row)
                    protocol.observe(row, query, queries)
                ended = row["kind"] == "end"
                if row["kind"] == "resources":
                    bindings.initialized(row)
                    effects.requests = [request["id"] for request in row["requests"]]
                elif row["kind"] == "notification_declared" or (row["kind"] == "native_protocol" and row["record"]["phase"] == "wire"):
                    group = row["identity"]["group"] if row["kind"] == "notification_declared" else row["group"]
                    event = row["event"] if row["kind"] == "notification_declared" else row["record"]["event"]
                    rank, = [rank for rank, owner in groups.items() if owner == group]
                    if event in event_ranks:
                        raise ValueError("external completion reuses an execution event identity")
                    event_ranks[event] = rank
                    predicted_dependencies[rank].add_events([event])
                elif row["kind"] == "resource":
                    bindings.released(row)
                elif row["kind"] == "binding":
                    bindings.bound(row)
                elif row["kind"] == "state_binding":
                    bindings.lifetimes.state_bound(row)
                if row["kind"] == "edge":

                    # CPU wire and wait edges have their own checked owner; every edge still enters the full graph below.
                    if row["relation"] == "communication" and (row["source"], row["target"]) not in cpu_protocol.communication_edges:
                        connections.communication_edges.add((row["source"], row["target"]))
                    if row["relation"] == "signal":
                        connections.signal_edges.add((row["source"], row["target"]))
                    source_rank, target_rank = event_ranks[row["source"]], event_ranks[row["target"]]
                    if source_rank == target_rank:
                        predicted_dependencies[source_rank].connect(row["source"], row["target"], row["relation"])
                    else:
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
                target = GraphContents(graph)
                conditioned_scope = condition_inputs.graph(groups[rank], row, target)
                if cpu_service:

                    # Runtime protocol intervals are independently checked before joining action causality.
                    continue
                group = ActionIdentity.group(graph, ranks[rank])
                try:
                    identity = ActionIdentity.record(root, group)
                except ValueError as error:
                    identity = {"group": group, "event": events[port["event"]], "action": root["role"]}
                    self.finding("missing_predicted_identity", identity, {"reason": str(error)})
                key = target.structure_key(0, identity, controls) if "work" in identity else ActionIdentity.structure_key(identity)
                predicted_counts[key] += 1
                full_key = ActionIdentity.logical_key(identity)
                full_predicted_counts[full_key] += 1
                predicted_classes[full_key].add(key)
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
                                content_match, snapshot if key in varied else None, conditioned_scope))
        condition_scope = condition_inputs.finish()
        checked_prefix = condition_scope["verified_prefix"]
        for key, (_, _, identity) in expected.items():
            if not predicted_counts[key]:
                self.finding("unmatched_native" if ended else "native_tail_unexecuted", identity, {})
        for key, indices in full_occurrences.items():
            if not full_predicted_counts[key] and any(predicted_counts[value] for value in native_classes[key]):
                self.finding("unpaired_native_context", json.loads(key), {"observed_calls": len(indices)})
            if len(indices) > 1 and not checked_prefix:
                self.finding("ambiguous_native_identity", json.loads(key), {
                    "observed_calls": len(indices), "predicted_calls": full_predicted_counts[key],
                    "reason": "complete logical context repeats; structure is checked independently",
                })
        for key, count in full_predicted_counts.items():
            if not full_occurrences[key] and predicted_classes[key] & expected.keys():
                self.finding("unpaired_predicted_context", json.loads(key), {"predicted_calls": count})
            if count > 1 and len(full_occurrences[key]) <= 1 and not checked_prefix:
                self.finding("ambiguous_predicted_identity", json.loads(key), {"predicted_calls": count})
        if not ended:
            self.finding("incomplete_execution", {}, {})
        if len(completed) != len(event_ranks):
            self.finding("unfinished_events", {}, {"events": len(event_ranks) - len(completed)})

        # Dynamic waits and state hazards are recorded after graph insertion; compare only after reading the whole execution.
        for graph in predicted_dependencies.values():
            graph.prepare()
        pairs, signatures, checks = {}, {}, []
        dependency_checks, dependency_matches = Counter(), Counter()
        actions, repeated_groups = [], {}
        for key, rank, identity, source, index, target_key, predicted, content_match, snapshot, conditioned_scope in pending:
            full_key = ActionIdentity.logical_key(identity)
            indices = [index for index in full_occurrences[full_key] if native_scope_keys[rank, index] == key]
            unique = len(indices) == len(full_occurrences[full_key]) == full_predicted_counts[full_key] == 1
            paired_index = conditioned_scope
            if paired_index is None and unique and ActionIdentity.occurrence_anchored(identity):
                paired_index = indices[0]
            paired = paired_index is not None
            if unique and not paired:
                self.finding("unpaired_native_context", identity, {
                    "observed_calls": 1, "predicted_calls": 1,
                    "reason": "repeatable read or no request work anchor; common structure is checked without occurrence pairing",
                })

            # A shared structure is not an occurrence. Pair with its own source scope, never the class representative.
            native = ActionBoundaries(source, paired_index if paired else index).ranked(rank)
            predicted = predicted.ranked(rank)
            if paired:
                if content_match is None:
                    content_match = self.compare_contents(identity, source.snapshot(paired_index, identity["group"]), snapshot)
                    content_checks[key] += 1
                    content_matches[key] += content_match
            checks.append((key, rank, identity, native, predicted, paired))

            # Only occurrence pairs may identify external producers, query results and communication rounds.
            if not paired:
                repeated_key = rank, full_key, key
                if repeated_key not in repeated_groups:
                    repeated_groups[repeated_key] = identity, [ActionBoundaries(source, index).ranked(rank) for index in indices], []
                repeated_groups[repeated_key][2].append(predicted)
                continue
            pairs[identity["group"], paired_index] = target_key
            paired_counts[key] += 1
            actions.append((identity, native, predicted))
            compared += 1
            matched += content_match
        pending.clear()

        connections.compare_signals(pairs)
        connections.compare_predicted_communication(predicted_dependencies)
        connections.compare_communication(pairs)
        predicted_links = {((event_ranks[source], source), (event_ranks[target], target), relation)
                           for source, target, relation in cross_rank_edges}
        actual_graph = native_case.graph
        predicted_graph = cpu_protocol.prediction(predicted_dependencies, predicted_links, complete=ended)
        connections.check_predicted_readiness(predicted_graph, event_ranks)
        connections.compare_values(predicted_graph, pairs)
        cpu_scope = cpu_protocol.report()
        order_ready = (connections.counts["native_communication_protocol_complete"]
                       and connections.counts["predicted_communication_protocol_complete"] and cpu_scope["complete"])
        if order_ready:
            for key, rank, identity, native, predicted, paired in checks:
                if key not in signatures:
                    source, index, _ = expected[key]
                    actual = actual_graph.signature(ActionBoundaries(source, index).ranked(rank))
                    signatures[key] = actual

                    # An alternate route through another rank can supply any local boundary dependency.
                    for index in occurrences[key][1:]:
                        repeated = actual_graph.signature(ActionBoundaries(source, index).ranked(rank))
                        if next(actual.differences(repeated), None) is not None:
                            signatures[key] = None
                            break
                actual = signatures[key]
                if paired and actual is None:
                    actual = actual_graph.signature(native)
                if actual is not None:
                    dependency_checks[key] += 1
                    different = False
                    for category, detail in actual.differences(predicted_graph.signature(predicted)):
                        self.finding(category, identity, detail)
                        different = True
                    dependency_matches[key] += not different
                    matched_dependencies += paired and not different
            ordering.compare(actual_graph, predicted_graph, actions)
            ordering.compare_repeated(actual_graph, predicted_graph, actions, repeated_groups)
        else:
            self.finding("ordering_evidence", {}, {
                "reason": "all-rank causal comparison needs complete model communication and CPU protocols",
                "native_model_protocol": connections.counts["native_communication_protocol_complete"],
                "predicted_model_protocol": connections.counts["predicted_communication_protocol_complete"],
                "cpu_protocol": cpu_scope["complete"],
            })
        bindings.compare(pairs)
        bindings.lifetimes.compare(predicted_graph, event_ranks)
        stream_scope = stream_synchronization.finish()
        for category, identity, detail in (*connections.findings, *bindings.findings, *bindings.lifetimes.findings,
                                          *ordering.findings, *cpu_protocol.findings, *stream_synchronization.findings):
            self.finding(category, identity, detail)
        for category, identity, detail in effects.compare(complete=ended):
            self.finding(category, identity, detail)

        for category, identity, detail in (*state_values.compare(protocol_source), *action_results.compare(pairs)):
            self.finding(category, identity, detail)
        for category, identity, detail in (*native_polling.findings, *protocol.polling.findings):
            self.finding(category, identity, detail)

        input_delivery = ingress.compare(condition_inputs.input_pairs if checked_prefix else None, complete=ended)
        for category, identity, detail in ingress.findings:
            self.finding(category, identity, detail)
        for instance, result in input_delivery.items():
            if result["status"] != "matched":
                category = "native_evidence" if result["status"] == "missing_native_evidence" else "request_ingress"
                self.finding(category, {"instance": instance}, result)
            if result.get("input_reads", {}).get("status") == "different":
                self.finding("input_read", {"instance": instance}, result["input_reads"])

        recorded_queries = queries.finish(complete=ended)
        checked_cross_rank, unverified_cross_rank = Counter(), Counter()
        communication = set()
        if cpu_scope["complete"]:
            communication.update(cpu_protocol.communication_edges)
        if connections.counts["predicted_communication_protocol_complete"]:
            communication.update(connections.communication_edges)
        for source, target, relation in cross_rank_edges:

            # Edge ownership comes from completed protocol checks; cross-run action order is checked separately.
            covered = ((relation == "signal" and (source, target) in protocol.notification_edges)
                       or (relation == "communication" and (source, target) in communication))
            (checked_cross_rank if covered else unverified_cross_rank)[relation] += 1
        query_counts = Counter()
        for key, (result, identity) in native_reads.items():
            if key not in pairs:
                query_counts["unpaired_native"] += 1
                continue
            if pairs[key] not in queries.results:
                query_counts["unreturned_predicted"] += 1
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
        queries_incomplete = query_counts["unpaired_native"] or query_counts["unpaired_predicted"] or query_counts["unreturned_predicted"]
        unpaired_calls = {"native": observed_actions - compared, "predicted": sum(predicted_counts.values()) - compared}
        incomplete = (payload_incomplete or any(unpaired_calls.values()) or input_incomplete or metadata_incomplete or admission_incomplete
                      or connections_incomplete or batch_sync_incomplete or bindings_incomplete or queries_incomplete
                      or bool(effects.counts["state_slots"]["unobserved_native"]) or bool(unverified_cross_rank)
                      or not cpu_scope["complete"] or not ordering.qualified or condition_scope["status"] == "partial")
        report = {
            **self.outcome(incomplete),
            "native_case": native_evidence,
            "native_actions_checked": observed_actions, "predicted_actions_checked": sum(predicted_counts.values()),
            "actions_compared": compared,
            "actions_contents_matched": matched,
            "actions_dependencies_matched": matched_dependencies,
            "action_checks": {
                "predicted_occurrences": sum(predicted_counts.values()),
                "contents": {"checked": sum(content_checks.values()), "matched": sum(content_matches.values())},
                "dependencies": {"checked": sum(dependency_checks.values()), "matched": sum(dependency_matches.values())},
            },
            "declared_control_classes": len(controls),
            "unpaired_calls": unpaired_calls,
            "counts": dict(self.counts), "findings": self.findings,
            "connections": connections.counts,
            "cpu_protocol": cpu_scope,
            "batch_synchronization": dict(connections.batch_sync.counts),
            "action_order": {**ordering.counts, "graph": "all_ranks_model_cpu_pd_and_state",
                             "coverage": "checked_scope" if ordering.qualified else "unqualified",
                             "scope": "Project full all-rank paths onto already-paired named events, preserving aliases and paths through unpaired intermediates. Compare each order's minimal relations to the opposite reachable order; both directions together retain strict order equality. A differing relation is not a proven core defect, and an opposite reverse path forbids simply adding that edge. No new occurrence pairing, timing or state-safety qualification."},
            "repeated_call_order": {**ordering.repeated_counts,
                                    "scope": "Sets of all-rank causal positions relative to uniquely paired call entries/returns in the same joined graphs. Counts, payloads, effects, cross-rank rounds and timing are not paired; no source occurrence is selected by matching its position."},
            "physical_bindings": bindings.counts,
            "state_lifetimes": bindings.lifetimes.report(),
            "stream_synchronization": stream_scope,
            "resource_effects": effects.counts,
            "state_values": state_values.counts,
            "action_results": dict(action_results.counts),
            "query_results_recorded": recorded_queries,
            "execution_times": execution_times.finish(complete=ended),
            "condition_inputs": condition_scope,
            "completion_reads_checked": queries.completion_reads,
            "completion_notifications_checked": queries.notifications,
            "pd_read_prerequisites": protocol.finish(),
            "native_protocol_readiness": protocol_readiness,
            "native_polling_operands": native_polling.operand_counts(),
            "native_admission_decisions": native_polling.commit_counts(),
            "query_outcomes": dict(query_counts),
            "request_ingress": input_delivery,
            "repeated_calls": [{"work": expected[key][2], "classification": "declared_control" if key in controls else "structural_class",
                                "paired_calls": paired_counts[key],
                                "observed_calls": len(occurrences[key]), "predicted_calls": predicted_counts[key],
                                "source_contents_uniform": key not in varied,
                                "contents_checked": content_checks[key], "contents_matched": content_matches[key],
                                "source_dependencies_uniform": signatures[key] is not None if key in signatures else None,
                                "dependencies_checked": dependency_checks[key], "dependencies_matched": dependency_matches[key]}
                               for key in repeated],
            "cross_rank_dependencies": {"protocol_checked_edges": dict(checked_cross_rank), "unverified_edges": dict(unverified_cross_rank),
                                        "scope": "Run-local edges owned by complete model/CPU protocols or independently checked PD notifications. Cross-run named-action causality is checked separately; private CPU messages are not paired."},
            "not_checked": [detail for missing, detail in (
                (queries_incomplete, "unpaired query outcomes"), (input_incomplete, "unpaired external input reads"),
                (bool(unverified_cross_rank), "cross-rank causality outside verified communication and notification protocols"),
                (any(unpaired_calls.values()), "per-occurrence correspondence of calls without an independently checked witness"),
            ) if missing] + ["exact PD notification arrival times", "unnamed internal event topology",
                            "private intra-call resource mutations and tensor hazards; native in-flight resource reuse outside observed cases",
                            "PD unused padding and unsupported multimodal/logprob/speculative values; controller output values outside recorded progress and publication fields"],
            "scope": "Every invocation retains its run-local identity; cross-run occurrence pairs require either unique full logical work anchors or a checked consumed native prefix independently reconstructed from native CPU observations, protocol posts and input reads. Every actual consumed identity and source reference binds its instantiated graph; positions alone never establish correspondence. Without this witness, repeatable reads and unanchored control calls remain unpaired even if observed once. Logical matches retain ancestor contexts and exclude time-only observation placement. Structural classes omit ancestor state for common-content checks. Native ABIs whose every observed declaration is independently verified host-only also share CPU contents across queue snapshots and continuation cuts; their predicted body never selects a class, and logical/time identities remain unchanged. Classes never identify external producers or causal boundaries. Contents and within-action boundary reachability/work placement are checked separately for every predicted occurrence when their respective observed signatures are uniform. This does not pair external producers, effects, query outcomes or communication rounds. Every nonempty predicted external data input is separately checked against its own published storage/version export and the shared all-rank causal graph. Every explicit signal wait is checked against its actual generation and recorded direct signal edge; completion queries do not acquire a wait. These run-local checks do not require native occurrence pairs. Signal cross-run matched counts concern producer identity. All predicted communication edges are separately checked against run-local channel participants and causal order, including repeated calls; this does not establish cross-run occurrence pairing or payload correctness. A qualified full-context pair uses its own observed scope even when the structural class varies; unpaired variation remains unverified. No signature is selected by resemblance to the prediction. Cross-action order, data writers/export readiness and native protocol participant dependencies retain their independent checks. All causal comparisons use the same per-run all-rank graphs, including unnamed intermediates, dynamic prediction edges and verified model/CPU communication. A path may cross ranks and return. Missing protocol coverage withholds causal qualification rather than proving a negative from an isolated rank. Event numbers, raw edge counts and measured times are not matched. Differences require diagnosis, not automatic attribution to core bugs.",
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
