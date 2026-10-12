#include "markov/trace_graph/modules/hicache/runtime/write_host.hpp"
#include "markov/trace_graph/core/dag_builder.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/modules/hicache/fact.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_expansion.hpp"
#include "markov/trace_graph/source_dag/call_regions.hpp"
#include <algorithm>
#include <set>
#include <span>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::runtime {

std::map<std::pair<std::string, std::string>, std::string> observe_write_controller_scopes(const patch::HiCacheSourceDagIndex & source) {
    // Prelude facts already survive window trimming for state reconstruction.
    // Read only identity/enclosure here; never import their CPU/device work.
    struct Call {
        std::string role, pid, tid, scope;
        uint64_t begin, end;
    };
    std::vector<Call> calls;
    const auto relevant = [](const auto & role) { return role == "commit_device_to_host_io_observed" || role == "commit_device_to_host_enqueue_observed"; };
    for (const auto & fact : source.fact_nodes())
        if (fact.phase == "end" && relevant(fact.fact_role))
            calls.push_back({ fact.fact_role, fact.pid, fact.tid, fact.cache_scope, fact.timestamp_us, fact.timestamp_us + fact.duration_us });
    for (const auto & event : source.graph().prelude_context_events()) {
        if (event.arg("phase") != "end") continue;
        const auto role = parse_hicache_fact_metadata(event).role;
        if (relevant(role)) calls.push_back({ role, event.pid, event.tid, event.arg("cache_scope"), event.ts, event.ts + event.dur });
    }
    std::map<std::pair<std::string, std::string>, std::string> scopes;
    for (const auto & call : calls) {
        if (call.role != "commit_device_to_host_io_observed" || call.scope.empty()) continue;
        for (const auto & tree : calls) {
            if (tree.role != "commit_device_to_host_enqueue_observed" || tree.scope.empty() || tree.pid != call.pid || tree.tid != call.tid
                || tree.begin > call.begin || tree.end < call.end)
                continue;
            const auto [at, inserted] = scopes.emplace(std::pair{ call.pid, call.scope }, tree.scope);
            if (!inserted && at->second != tree.scope) throw std::runtime_error("Write controller maps to multiple cache trees");
        }
    }
    return scopes;
}

const patch::HiCacheSourceFactNode & observe_write_envelope(const patch::HiCacheSourceDagIndex & source, const std::string & pid, const std::string & tid,
                                                            uint64_t begin, uint64_t end) {
    const patch::HiCacheSourceFactNode * outer = nullptr;
    // An admitted request lifecycle may finish after the HTTP endpoint. Its
    // write record and enclosing call live in the same preserved tail context;
    // looking up that envelope must not extend the measured request window.
    for (const auto facts : { std::span<const patch::HiCacheSourceFactNode>(source.fact_nodes()), source.tail_context_facts() }) {
        for (const auto & fact : facts) {
            if (fact.fact_role != "commit_device_to_host_enqueue_observed" || fact.phase != "end" || fact.pid != pid || fact.tid != tid
                || fact.timestamp_us > begin || fact.timestamp_us + fact.duration_us < end)
                continue;
            if (outer) throw std::runtime_error("Write submission has multiple enclosing write_backup calls");
            outer = &fact;
        }
    }
    if (!outer)
        throw std::runtime_error("Write submission lacks its enclosing write_backup call: pid=" + pid + " tid=" + tid + " begin_us=" + std::to_string(begin)
                                 + " end_us=" + std::to_string(end));
    return *outer;
}

HiCacheHostTemplate observe_write_host_template(const patch::HiCacheSourceDagIndex & source, const patch::HiCacheIoOperationRecord & write) {
    if (write.kind != patch::HiCacheIoOperationKind::WriteDeviceToHost) throw std::invalid_argument("Write host template requires a D2H call");
    const auto & outer = observe_write_envelope(source, write.pid, write.tid, write.source_start_us, write.source_end_us);
    auto host = observe_host_template(source, write.pid, write.tid, outer.timestamp_us, outer.timestamp_us + outer.duration_us);
    host.write_back = outer.write_back;
    return host;
}

HiCacheHostTemplate observe_host_template(const patch::HiCacheSourceDagIndex & source, const std::string & pid, const std::string & tid, uint64_t begin,
                                          uint64_t end) {
    if (end <= begin) throw std::invalid_argument("Host template requires a nonempty interval");
    HiCacheHostTemplate result{ .main = source.call_interval_ownership(pid, tid, begin, end - begin) };
    if (result.main.status != "ready" || (result.main.owned_node_ids.empty() && result.main.owned_gap_slices.empty()) || result.main.has_node_overlap)
        throw std::runtime_error("Host template lacks nonoverlapping main-thread leaves: pid=" + pid + " at_us=" + std::to_string(begin)
                                 + " reason=" + result.main.reason);
    const auto & graph = source.graph();
    const std::set<size_t> main(result.main.owned_node_ids.begin(), result.main.owned_node_ids.end());
    // A runtime worker can execute after its caller returns. Follow its
    // submission identity, not overlap with the caller's wall-clock envelope.
    std::set<size_t> seen = main;
    auto pending = result.main.owned_node_ids;
    for (size_t i = 0; i < pending.size(); ++i) {
        for (const auto id : source.outgoing_edge_ids(pending[i])) {
            const auto & edge = graph.edge(id);
            const auto & node = graph.node(edge.dst);
            if (!edge.active || edge.kind != core::DagEdgeKind::Correlation || !node.active || !node.is_cpu || seen.contains(edge.dst)) continue;
            const auto & event = graph.event_for_node(edge.dst);
            if (event.pid == pid && event.tid == tid) throw std::runtime_error("Submission identity escapes the main-thread call");
            if (node.gpu_id != graph.node(pending[i]).gpu_id) throw std::runtime_error("Worker submission crosses ranks");
            seen.insert(edge.dst);
            pending.push_back(edge.dst);
            result.worker_nodes.push_back(edge.dst);
        }
    }
    for (const auto node : seen)
        for (const auto id : source.incoming_edge_ids(node)) {
            const auto & edge = graph.edge(id);
            if (edge.active && edge.kind == core::DagEdgeKind::Sync && graph.node(edge.src).active && !graph.node(edge.src).is_cpu)
                result.device_wait_edges.push_back(id);
        }
    return result;
}

std::vector<HiCacheEvictionRegion> observe_eviction_regions(const patch::HiCacheSourceDagIndex & source, const patch::HiCacheSourceFactNode & eviction) {
    using Kind = HiCacheEvictionRegion::Kind;
    if (eviction.fact_role != "capacity_result_observed" || eviction.phase != "end" || !eviction.duration_us)
        throw std::invalid_argument("Eviction regions require a complete source evict envelope");
    const auto begin = eviction.timestamp_us, end = begin + eviction.duration_us;
    std::vector<HiCacheEvictionRegion> calls;
    const auto add = [&](Kind kind, const std::string & pid, const std::string & tid, uint64_t start, uint64_t duration, uint64_t tokens = 0) {
        if (pid != eviction.pid || tid != eviction.tid || start >= end || start + duration <= begin) return;
        if (start < begin || start + duration > end || !duration) throw std::runtime_error("Eviction child crosses its source envelope");
        calls.push_back({ kind, start, start + duration, tokens });
    };
    for (const auto & fact : source.fact_nodes())
        if (fact.fact_role == "commit_device_to_host_enqueue_observed" && fact.phase == "end")
            add(Kind::WriteBackup, fact.pid, fact.tid, fact.timestamp_us, fact.duration_us);
    for (const auto & event : source.graph().runtime_observations()) {
        if (event.name == "runtime.hicache.write_completion" && event.arg("blocking") == "true")
            add(Kind::BlockingCheck, event.pid, event.tid, event.ts, event.dur);
        if (event.name != "runtime.hicache.device_release_backup" && event.name != "runtime.hicache.device_release_regular") continue;
        if (event.pid != eviction.pid || event.tid != eviction.tid || event.ts >= end || event.ts + event.dur <= begin) continue;
        const auto tokens = core::parse_u64(event.arg("released_tokens"));
        if (event.arg("status") != "returned" || !tokens) throw std::runtime_error("Device release lacks its successful scalar return");
        const auto kind = event.name == "runtime.hicache.device_release_backup" ? Kind::ReleaseBackup : Kind::ReleaseRegular;
        add(kind, event.pid, event.tid, event.ts, event.dur, *tokens);
    }
    std::ranges::sort(calls, {}, &HiCacheEvictionRegion::begin);
    std::vector<HiCacheEvictionRegion> regions;
    auto cursor = begin;
    for (const auto & call : calls) {
        if (call.begin < cursor) throw std::runtime_error("Eviction child calls overlap");
        if (cursor < call.begin) regions.push_back({ Kind::Control, cursor, call.begin });
        regions.push_back(call);
        cursor = call.end;
    }
    if (cursor < end) regions.push_back({ Kind::Control, cursor, end });
    // Whole setup/tail fragments run once; fragments between calls belong to
    // the corresponding loop. Empty selection has no separately observed
    // setup/iteration costs and must not donate either primitive.
    using Phase = HiCacheEvictionRegion::ControlPhase;
    const auto check = std::ranges::find(regions, Kind::BlockingCheck, &HiCacheEvictionRegion::kind);
    const auto check_index = static_cast<size_t>(check - regions.begin());
    for (size_t i = 0; i < regions.size(); ++i) {
        auto & region = regions[i];
        if (region.kind != Kind::Control) continue;
        if (i) region.preceding_kind = regions[i - 1].kind;
        if (i == 0) region.control_phase = (i + 1 == regions.size() || i + 1 == check_index) ? Phase::Empty : Phase::Setup;
        else if (i + 1 == regions.size()) region.control_phase = Phase::Finish;
        else if (i < check_index) region.control_phase = i + 1 == check_index ? Phase::SelectionEnd : Phase::Selection;
        else region.control_phase = i == check_index + 1 ? Phase::ReleaseStart : Phase::ReleaseNext;
    }
    return regions;
}

HiCacheHostExpansion prepare_host_expansion(const patch::HiCacheSourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues,
                                            const HiCacheHostTemplate & host, std::string_view prefix, std::span<const size_t> callback_points,
                                            bool retain_collective_completions) {
    const auto & graph = source.graph();
    HiCacheHostExpansion plan;
    auto & mapped = plan.source_nodes;
    const auto copy = [&](size_t id) {
        const auto & node = graph.node(id);
        return core::DagSyntheticNodeSpec{ .name = std::string(prefix) + graph.event_for_node(id).name,
                                           .category = "hicache_patch",
                                           .is_cpu = node.is_cpu,
                                           .lane_key = std::string(graph.node_lane_key(id)),
                                           .duration = graph.cpu_service_node_duration(id) };
    };
    struct Piece {
        uint64_t at;
        std::optional<size_t> node;
        uint64_t gap;
    };
    std::vector<Piece> pieces;
    auto main_nodes = host.main.owned_node_ids;
    if (!callback_points.empty()) {
        std::set<size_t> selected(main_nodes.begin(), main_nodes.end());
        selected.insert(callback_points.begin(), callback_points.end());
        const auto & sample = graph.event_for_node(callback_points.front());
        const auto lane = source.cpu_nodes_on_lane(sample.pid, sample.tid);
        const auto first = std::ranges::lower_bound(lane, host.main.interval_start_us, {}, [&](size_t id) { return graph.event_for_node(id).ts; });
        main_nodes.clear();
        for (auto at = first; at != lane.end() && graph.event_for_node(*at).ts < host.main.interval_end_us; ++at)
            if (selected.erase(*at)) main_nodes.push_back(*at);

        if (!selected.empty()) throw std::runtime_error("Retained callbacks must belong to the copied call's CPU interval");
        for (const auto point : callback_points)
            if (graph.node(point).duration || graph.node(point).cpu_gap_after || graph.scope_gap_duration(point))
                throw std::runtime_error("Retained callback point must not duplicate separately owned service or gaps");
    }
    for (const auto id : main_nodes) pieces.push_back({ graph.event_for_node(id).ts, id, 0 });
    for (const auto & gap : host.main.owned_gap_slices) {
        const auto & event = graph.event_for_node(gap.owner_node_id);
        const auto retained = graph.observed_cpu_gap_ranges(gap.owner_node_id);
        if (!retained) throw std::runtime_error("Host expansion cannot migrate a remodeled gap without source coordinates");

        // Nested replacements can remove only part of an observed gap. The
        // old timestamps still enclose that time, but they do not authorize
        // copying it again into the enclosing replacement.
        for (const auto & [lo, hi] : *retained) {
            const auto begin = std::max(lo, gap.owned_start_us), end = std::min(hi, gap.owned_end_us);
            if (begin >= end) continue;

            const auto service = graph.cpu_service_cost().duration({ event.pid, event.tid }, begin, end);
            pieces.push_back({ begin, std::nullopt, service });
        }
    }
    // Equal-tick submissions retain the source lane's order. Sorting only by
    // timestamp without stability can reverse zero-duration enqueue points.
    std::ranges::stable_sort(pieces, {}, &Piece::at);
    if (pieces.empty()) throw std::runtime_error("Host expansion needs observed leaves or gaps");
    const auto lane_node = main_nodes.empty() ? host.main.owned_gap_slices.front().owner_node_id : main_nodes.front();
    for (const auto & piece : pieces) {
        auto spec = piece.node ? copy(*piece.node)
                               : core::DagSyntheticNodeSpec{ .name = std::string(prefix) + "retained host gap",
                                                             .category = "hicache_patch",
                                                             .lane_key = std::string(graph.node_lane_key(lane_node)),
                                                             .cpu_gap_after = piece.gap };
        const auto id = plan.nodes.size();
        plan.nodes.push_back({ std::move(spec) });
        if (piece.node) mapped.emplace(*piece.node, id);
        if (id) plan.edges.push_back({ id - 1, id, core::DagEdgeKind::Sequential });
    }
    // A final zero node also accounts for a retained trailing CPU gap.
    plan.host_return = plan.nodes.size();
    plan.nodes.push_back({
        { .name = std::string(prefix) + "host return", .lane_key = plan.nodes.front().work.lane_key }
    });
    plan.edges.push_back({ plan.host_return - 1, plan.host_return, core::DagEdgeKind::Sequential });

    // Correlation may identify only the first span of a submitted task. Keep
    // its same-task continuations too, but never copy the next FIFO job. This
    // is the same ownership boundary used when removing the original call.
    auto worker_nodes = host.worker_nodes;
    std::set<size_t> workers(worker_nodes.begin(), worker_nodes.end());
    for (size_t i = 0; i < worker_nodes.size(); ++i) {
        const auto task = queues.node_task.at(worker_nodes[i]);
        if (task == core::DagNode::kNoNode) continue;

        for (const auto edge_id : source.outgoing_edge_ids(worker_nodes[i])) {
            const auto & edge = graph.edge(edge_id);
            if (!edge.active || edge.kind != core::DagEdgeKind::Sequential || !graph.node(edge.dst).active || queues.node_task.at(edge.dst) != task) continue;

            if (workers.insert(edge.dst).second) worker_nodes.push_back(edge.dst);
        }
    }

    std::map<size_t, std::vector<size_t>> jobs;
    for (const auto node : worker_nodes) {
        const auto task = queues.node_task.at(node);
        if (task == core::DagNode::kNoNode) {
            const auto & event = graph.event_for_node(node);
            throw std::runtime_error("Host worker is not a recognized queue task: template=" + std::string(prefix) + " node=" + std::to_string(node)
                                     + " name=" + event.name + " pid=" + event.pid + " tid=" + event.tid + " at_us=" + std::to_string(event.ts)
                                     + " correlation=" + event.arg("correlation_id"));
        }
        jobs[task].push_back(node);
    }
    for (auto & [task_id, nodes] : jobs) {
        const auto & task = queues.tasks.at(task_id);
        std::ranges::sort(nodes, {}, [&](size_t id) { return std::pair{ graph.event_for_node(id).ts, id }; });
        if (nodes.front() != task.first || nodes.back() != task.last || !mapped.contains(task.submission))
            throw std::runtime_error("Write template owns only part of a runtime queue job");
        uint64_t previous_end = graph.event_for_node(task.first).ts;
        std::optional<size_t> previous;
        for (const auto node : nodes) {
            const auto & event = graph.event_for_node(node);
            const auto gap = event.ts > previous_end ? graph.cpu_service_cost().duration({ event.pid, event.tid }, previous_end, event.ts) : 0;
            auto spec = copy(node);
            spec.duration = core::checked_add_u64(spec.duration, gap, "Write worker cost overflow");
            if (!previous) spec.cpu_task_ready_delay_us = graph.cpu_service_ready_delay(task.first, task.ready_delay_us);
            previous_end = std::max(previous_end, event.ts + event.dur);
            const auto local = plan.nodes.size();
            mapped.emplace(node, local);
            plan.nodes.push_back({ std::move(spec), mapped.at(task.submission), task.first, previous });
            if (previous) plan.edges.push_back({ *previous, local, core::DagEdgeKind::Sequential });
            previous = local;
        }
    }

    auto pending = host.main.owned_node_ids;
    pending.insert(pending.end(), worker_nodes.begin(), worker_nodes.end());
    std::set<size_t> devices;
    for (size_t i = 0; i < pending.size(); ++i)
        for (const auto id : source.outgoing_edge_ids(pending[i])) {
            const auto & edge = graph.edge(id);
            if (!edge.active || edge.kind != core::DagEdgeKind::Correlation || !graph.node(edge.dst).active || graph.node(edge.dst).is_cpu) continue;
            if (devices.insert(edge.dst).second) pending.push_back(edge.dst);
        }
    const auto placeholders = source_dag::interior_stream_placeholders(source, devices);
    devices.insert(placeholders.begin(), placeholders.end());
    for (const auto node : devices) {
        mapped.emplace(node, plan.nodes.size());
        plan.nodes.push_back({ copy(node) });
    }

    std::map<size_t, std::vector<size_t>> streams;
    for (const auto node : devices) streams[graph.node(node).lane_id].push_back(node);
    for (const auto & [lane, nodes] : streams) {
        std::vector<size_t> first, last;
        for (const auto node : nodes) {
            bool has_before = false, has_after = false;
            for (const auto id : source.incoming_edge_ids(node)) {
                const auto & edge = graph.edge(id);
                has_before |= edge.active && edge.kind == core::DagEdgeKind::Stream && devices.contains(edge.src);
            }
            for (const auto id : source.outgoing_edge_ids(node)) {
                const auto & edge = graph.edge(id);
                has_after |= edge.active && edge.kind == core::DagEdgeKind::Stream && devices.contains(edge.dst);
            }
            if (!has_before) first.push_back(node);
            if (!has_after) last.push_back(node);
        }
        if (first.size() != 1 || last.size() != 1) {
            std::string details = " lane=" + std::to_string(lane) + " observed_begin_us=" + std::to_string(host.main.interval_start_us)
                                  + " observed_end_us=" + std::to_string(host.main.interval_end_us) + " starts=" + std::to_string(first.size())
                                  + " ends=" + std::to_string(last.size());
            for (size_t i = 0; i < std::min(first.size(), size_t{ 4 }); ++i) {
                const auto & event = graph.event_for_node(first[i]);
                details += " start=" + std::to_string(first[i]) + ":" + event.name + "@" + std::to_string(event.ts);
            }
            throw std::runtime_error("Write template has a disconnected device stream" + details);
        }
        plan.streams.push_back({ first.front(), mapped.at(first.front()), mapped.at(last.front()) });
    }
    for (const auto & [node, local] : mapped)
        for (const auto id : source.incoming_edge_ids(node)) {
            const auto & edge = graph.edge(id);
            if (!edge.active || !graph.node(edge.src).active) continue;
            if (const auto from = mapped.find(edge.src); from != mapped.end()) {
                if (from->second == local || edge.kind == core::DagEdgeKind::Sequential) continue;
                if (plan.nodes[local].submission == from->second && edge.kind == core::DagEdgeKind::Correlation) continue;
                plan.edges.push_back({ from->second, local, edge.kind == core::DagEdgeKind::Correlation ? core::DagEdgeKind::Mutation : edge.kind });
            }
            else if (edge.kind == core::DagEdgeKind::Sequential || edge.kind == core::DagEdgeKind::Stream) continue;
            else if (retain_collective_completions && edge.kind == core::DagEdgeKind::HCCL && !graph.node(edge.src).is_cpu && !graph.node(node).is_cpu)
                plan.collective_completions.push_back({ edge.src, local });
            else if (edge.kind == core::DagEdgeKind::Sync && !graph.node(edge.src).is_cpu && core::synchronizes_device_frontier(graph.event_for_node(node)))
                plan.waits.push_back({ edge.src, local });
            else if (edge.kind == core::DagEdgeKind::Sync && graph.event_for_node(edge.src).name == "EVENT_RECORD" && !graph.node(edge.src).is_cpu
                     && (core::synchronizes_recorded_event(graph.event_for_node(node))
                         || (!graph.node(node).is_cpu
                             && (graph.event_for_node(node).name == "EVENT_WAIT" || graph.event_for_node(node).name == "logical_event_wait"))))
                plan.event_waits.push_back({ edge.src, local });
            else
                throw std::runtime_error("Host template has an unresolved external dependency: " + graph.event_for_node(edge.src).name + " -> "
                                         + graph.event_for_node(node).name);
        }
    // Main-thread pieces were copied in call order. Source node IDs can come
    // from different trace files, so they must not order queued event ACKs.
    std::ranges::stable_sort(plan.event_waits, {}, &HiCacheHostExpansion::Wait::consumer);
    return plan;
}

HiCacheWriteExpansion prepare_write_expansion(const patch::HiCacheSourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues,
                                              const patch::HiCacheIoOperationRecord & write, const HiCacheHostTemplate & host, size_t completion) {
    HiCacheWriteExpansion plan{ prepare_host_expansion(source, queues, host, "target write: ") };
    if (!plan.event_waits.empty()) throw std::runtime_error("Write template needs an explicit event-completion binding");
    const auto & graph = source.graph();
    const auto & mapped = plan.source_nodes;
    const std::set<size_t> payload(write.device_transfer_node_ids.begin(), write.device_transfer_node_ids.end());
    if (!mapped.contains(completion) || payload.empty()
        || !std::ranges::all_of(payload, [&](size_t id) { return mapped.contains(id) && !graph.node(id).is_cpu; }))
        throw std::runtime_error("Write template does not own its complete device submission closure");
    plan.completion = mapped.at(completion);
    for (const auto id : payload) {
        const auto bytes = core::parse_u64(graph.event_for_node(id).arg("size(B)")).value_or(0);
        if (!bytes) throw std::runtime_error("Write template payload has no byte geometry");
        plan.payload_bytes = core::checked_add_u64(plan.payload_bytes, bytes, "Write template bytes overflow");
        plan.payload.emplace(mapped.at(id), bytes);
    }
    for (const auto & stream : plan.streams)
        if (graph.node(stream.source_node).lane_id == graph.node(completion).lane_id) plan.write_start = stream.first;
    // Stream order, not arbitrary source IDs, identifies the first D2H payload.
    std::vector<size_t> payload_heads;
    for (const auto node : payload) {
        bool earlier = false;
        std::vector<size_t> pending{ node };
        std::set<size_t> visited;
        while (!pending.empty()) {
            const auto current = pending.back();
            pending.pop_back();
            if (!visited.insert(current).second) continue;
            for (const auto id : source.incoming_edge_ids(current)) {
                const auto & edge = graph.edge(id);
                if (!edge.active || edge.kind != core::DagEdgeKind::Stream || !mapped.contains(edge.src)) continue;
                if (payload.contains(edge.src)) earlier = true;
                else pending.push_back(edge.src);
            }
        }
        if (!earlier) payload_heads.push_back(node);
    }
    if (payload_heads.size() != 1) throw std::runtime_error("Write template has multiple payload heads");

    return plan;
}

} // namespace markov::trace_graph::modules::hicache::runtime
