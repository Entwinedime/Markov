#include "markov/trace_graph/modules/hicache/runtime/writes.hpp"
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::runtime {
namespace {

bool transparent_stream_placeholder(const patch::HiCacheSourceDagIndex & source, size_t node_id) {
    const auto & graph = source.graph();
    const auto & node = graph.node(node_id);
    const auto & event = graph.event_for_node(node_id);
    if (node.is_cpu || node.duration || event.dur || event.name != "PLACE_HOLDER_SQE") return false;

    for (const auto edge_id : source.incoming_edge_ids(node_id)) {
        const auto & edge = graph.edge(edge_id);
        if (edge.active && edge.kind != core::DagEdgeKind::Stream) return false;
    }
    for (const auto edge_id : source.outgoing_edge_ids(node_id)) {
        const auto & edge = graph.edge(edge_id);
        if (edge.active && edge.kind != core::DagEdgeKind::Stream) return false;
    }
    return true;
}

} // namespace

size_t HiCacheWrites::source_start_record(const patch::HiCacheSourceDagIndex & source, size_t source_fact) const {
    const auto & site = sources_.at(source_fact);
    const auto & graph = source.graph();
    auto current = site.transfers.front().first;
    const auto & order = source.device_stream_order(current);
    const auto lane = graph.node(current).lane_id;
    const auto rank = graph.node(current).gpu_id;
    std::set<size_t> visited;
    while (visited.insert(current).second) {
        if (graph.event_for_node(current).name == "EVENT_WAIT" || graph.event_for_node(current).name == "logical_event_wait") {
            std::optional<size_t> record;
            for (const auto id : source.incoming_edge_ids(current)) {
                const auto & edge = graph.edge(id);
                if (!edge.active || edge.kind != core::DagEdgeKind::Sync || !graph.node(edge.src).active) continue;
                const auto & node = graph.node(edge.src);
                if (node.is_cpu || node.gpu_id != rank || node.lane_id == lane || graph.event_for_node(edge.src).name != "EVENT_RECORD") continue;
                if (!source.device_submitted_in_call(edge.src, site.pid, site.tid, site.begin_us, site.end_us)) continue;
                if (record) throw std::runtime_error("Write start Wait has multiple compute Records");
                record = edge.src;
            }
            if (record) return *record;
            throw std::runtime_error("Write start Wait has no in-call compute Record");
        }
        const auto previous = order.previous(current);
        if (!previous || graph.node(*previous).is_cpu || graph.node(*previous).lane_id != lane || graph.node(*previous).gpu_id != rank) break;
        const auto & name = graph.event_for_node(*previous).name;
        if (name != "EVENT_WAIT" && name != "EVENT_RECORD" && name != "logical_event_wait") break;
        current = *previous;
    }
    throw std::runtime_error("Write template lacks its initial stream Wait");
}

HiCacheWriteStreamPosition observe_write_stream_position(const patch::HiCacheSourceDagIndex & source, size_t stream_node, const std::string & pid,
                                                         const std::string & tid, uint64_t at_us) {
    const auto & graph = source.graph();
    const auto & sample = graph.node(stream_node);
    if (sample.is_cpu) throw std::invalid_argument("Write stream template must identify a device resource");
    const auto & order = source.device_stream_order(stream_node);
    HiCacheWriteStreamPosition position;
    position.submission_site = core::DagObservedPoint{ pid, tid, at_us, sample.gpu_id };
    if (order.nodes.empty()) return position;
    std::optional<size_t> last_before, first_after;
    std::vector<std::pair<size_t, size_t>> unanchored;
    const auto & submissions = source.device_submission_anchors(stream_node, pid, tid);
    for (size_t ordinal = 0; ordinal < order.nodes.size(); ++ordinal) {
        const auto current = order.nodes[ordinal];
        const auto & anchors = submissions[ordinal];
        if (anchors.empty()) unanchored.emplace_back(ordinal, current);
        bool before = !anchors.empty(), after = !anchors.empty();
        for (const auto id : anchors) {
            const auto & event = graph.event_for_node(id);
            before &= event.ts < at_us && event.dur <= at_us - event.ts;
            after &= event.ts >= at_us;
        }
        if (!anchors.empty() && !before && !after) throw std::runtime_error("Write insertion overlaps a CPU submission");
        if (before) {
            if (position.after) throw std::runtime_error("Write insertion submission order disagrees with stream order");
            position.before = current;
            last_before = ordinal;
        }
        else if (after && !position.after) {
            position.after = current;
            first_after = ordinal;
        }
    }
    for (const auto & [ordinal, node] : unanchored) {
        // Stream order proves the remote prefix/suffix even when a runtime
        // marker lacks its own CPU correlation. Never guess across the cut.
        if ((last_before && ordinal < *last_before) || (first_after && ordinal > *first_after)) continue;
        // The unique Stream order is already proven by the source index.
        // A zero-service placeholder with no external dependency commutes
        // with inserted work at this cut. Keep the marker and its original
        // edges, choosing the cut after it rather than inventing a CPU anchor.
        // This follows the builder's microsecond-resolution wait convention;
        // its fractional source observation is not changed or used as a cost.
        if (transparent_stream_placeholder(source, node)) {
            position.before = node;
            continue;
        }
        const auto & event = graph.event_for_node(node);
        auto message = "Write insertion has an unanchored node at its cut: pid=" + pid + " at_us=" + std::to_string(at_us) + " node=" + std::to_string(node)
                       + " name=" + event.name + " source_us=" + std::to_string(event.ts) + " lane=" + std::to_string(graph.node(node).lane_id)
                       + " connection=" + event.arg("connection_id");
        for (const auto edge_id : source.incoming_edge_ids(node)) {
            const auto & edge = graph.edge(edge_id);
            if (!edge.active || !graph.node(edge.src).active || edge.kind != core::DagEdgeKind::Correlation) continue;
            const auto & parent = graph.event_for_node(edge.src);
            message += " submit=" + parent.name + ":" + parent.pid + "/" + parent.tid + "@" + std::to_string(parent.ts);
        }
        throw std::runtime_error(message);
    }
    return position;
}

} // namespace markov::trace_graph::modules::hicache::runtime
