#include "markov/trace_graph/modules/hicache/runtime/write_host.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/modules/hicache/fact.hpp"
#include <algorithm>
#include <set>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::runtime {

std::map<std::pair<std::string, std::string>, std::string> observe_write_controller_scopes(
    const patch::HiCacheSourceDagIndex & source) {
    // Prelude facts already survive window trimming for state reconstruction.
    // Read only identity/enclosure here; never import their CPU/device work.
    struct Call {
        std::string role, pid, tid, scope;
        uint64_t begin, end;
    };
    std::vector<Call> calls;
    const auto relevant = [](const auto & role) {
        return role == "commit_device_to_host_io_observed" || role == "commit_device_to_host_enqueue_observed";
    };
    for (const auto & fact : source.fact_nodes())
        if (fact.phase == "end" && relevant(fact.fact_role))
            calls.push_back({fact.fact_role, fact.pid, fact.tid, fact.cache_scope, fact.timestamp_us, fact.timestamp_us + fact.duration_us});
    for (const auto & event : source.graph().prelude_context_events()) {
        if (event.arg("phase") != "end") continue;
        const auto role = parse_hicache_fact_metadata(event).role;
        if (relevant(role)) calls.push_back({role, event.pid, event.tid, event.arg("cache_scope"), event.ts, event.ts + event.dur});
    }
    std::map<std::pair<std::string, std::string>, std::string> scopes;
    for (const auto & call : calls) {
        if (call.role != "commit_device_to_host_io_observed" || call.scope.empty()) continue;
        for (const auto & tree : calls) {
            if (tree.role != "commit_device_to_host_enqueue_observed" || tree.scope.empty()
                || tree.pid != call.pid || tree.tid != call.tid || tree.begin > call.begin || tree.end < call.end) continue;
            const auto [at, inserted] = scopes.emplace(std::pair{call.pid, call.scope}, tree.scope);
            if (!inserted && at->second != tree.scope)
                throw std::runtime_error("Write controller maps to multiple cache trees");
        }
    }
    return scopes;
}

const patch::HiCacheSourceFactNode & observe_write_envelope(
    const patch::HiCacheSourceDagIndex & source, const std::string & pid, const std::string & tid, uint64_t begin, uint64_t end) {
    const patch::HiCacheSourceFactNode * outer = nullptr;
    for (const auto & fact : source.fact_nodes()) {
        if (fact.fact_role != "commit_device_to_host_enqueue_observed" || fact.phase != "end" || fact.pid != pid || fact.tid != tid
            || fact.timestamp_us > begin || fact.timestamp_us + fact.duration_us < end)
            continue;
        if (outer) throw std::runtime_error("Write submission has multiple enclosing write_backup calls");
        outer = &fact;
    }
    if (!outer) throw std::runtime_error("Write submission lacks its enclosing write_backup call");
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
    HiCacheHostTemplate result{ .main = source.timing_interval_ownership(pid, tid, begin, end - begin) };
    if (result.main.status != "ready" || (result.main.owned_node_ids.empty() && result.main.owned_gap_slices.empty())
        || result.main.has_node_overlap)
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

} // namespace markov::trace_graph::modules::hicache::runtime
