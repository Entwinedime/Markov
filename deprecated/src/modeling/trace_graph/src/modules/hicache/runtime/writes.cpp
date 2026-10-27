#include "markov/trace_graph/modules/hicache/runtime/writes.hpp"
#include "markov/trace_graph/adapters/frameworks/sglang/hicache_observations.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/structure/missing_work.hpp"
#include <algorithm>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::runtime {

void HiCacheWrites::add_source(const adapters::sglang::HiCacheSourceIndex & source, const patch::HiCacheIoOperationRecord & record) {
    if (record.kind != patch::HiCacheIoOperationKind::WriteDeviceToHost || record.device_transfer_node_ids.empty())
        throw structure::MissingWork("Write site requires an observed D2H payload",
                                     {
                                         {            "operation",                                                             "device_to_host" },
                                         {                  "pid",                                                                   record.pid },
                                         {                  "tid",                                                                   record.tid },
                                         {      "source_begin_us",                                                       record.source_start_us },
                                         {        "source_end_us",                                                         record.source_end_us },
                                         {               "reason",                                                                record.reason },
                                         { "required_observation", "DMA direction, byte geometry and caller-to-device completion relationships" }
        });

    const auto & graph = source.graph();
    Source site;
    site.pid = record.pid;
    site.tid = record.tid;
    site.begin_us = record.source_start_us;
    site.end_us = record.source_end_us;
    std::set<size_t> remaining(record.device_transfer_node_ids.begin(), record.device_transfer_node_ids.end());
    // Find the first owned transfer by stream order, not device timestamps
    // (which can tie). A record may list tied transfers in either order.
    auto current = record.device_transfer_node_ids.front();
    const auto & order = source.device_stream_order(current);
    const auto lane = graph.node(current).lane_id;
    const auto rank = graph.node(current).gpu_id;
    std::set<size_t> ancestors;
    auto cursor = current;
    while (ancestors.insert(cursor).second) {
        const auto previous = order.previous(cursor);
        if (!previous) break;
        const auto & node = graph.node(*previous);
        const auto & event = graph.event_for_node(*previous);
        if (node.lane_id != lane || node.gpu_id != rank) break;
        if (remaining.contains(*previous)) current = *previous;
        else if (event.name != "EVENT_RECORD" && event.name != "EVENT_WAIT" && event.name != "logical_event_wait") break;
        cursor = *previous;
    }
    std::set<size_t> visited;
    while (visited.insert(current).second) {
        const auto & node = graph.node(current);
        const auto & event = graph.event_for_node(current);
        if (!node.active || node.is_cpu || node.lane_id != lane || node.gpu_id != rank)
            throw std::runtime_error("Write payload leaves its observed device stream");
        if (remaining.erase(current)) {
            const auto bytes = core::parse_u64(event.arg("size(B)"));
            if (event.arg("operation") != "device to host" || !bytes || !*bytes) throw std::runtime_error("Write payload lacks direction or byte geometry");
            site.transfers.emplace_back(current, *bytes);
        }
        else if (event.name != "EVENT_RECORD" && event.name != "EVENT_WAIT" && event.name != "logical_event_wait")
            throw std::runtime_error("Unowned device work interrupts a write payload");
        if (remaining.empty() && event.name == "EVENT_RECORD") {
            site.completion = current;
            break;
        }
        const auto next = order.next(current);
        if (!next) throw std::runtime_error("Write payload has no observed completion Record");
        current = *next;
    }
    if (!remaining.empty() || graph.event_for_node(current).name != "EVENT_RECORD")
        throw std::runtime_error("Write payload does not reach its completion Record");
    // The runtime worker may record the event after start_writing returns.
    // Ownership follows the original caller, not that delayed worker clock.
    if (!source.device_submitted_in_call(current, record.pid, record.tid, record.source_start_us, record.source_end_us))
        throw std::runtime_error("Write completion Record does not belong to this host submission: pid=" + record.pid + " tid=" + record.tid
                                 + " begin_us=" + std::to_string(record.source_start_us) + " end_us=" + std::to_string(record.source_end_us)
                                 + " completion_node=" + std::to_string(current));
    for (const auto & [node, bytes] : site.transfers)
        if (observed_transfers_.contains(node)) throw std::runtime_error("Write sites share a payload node");
    if (!sources_.emplace(record.timing_fact_node_id, site).second) throw std::runtime_error("Duplicate source write site");
    for (const auto & [node, bytes] : site.transfers) observed_transfers_.insert(node);
}

std::vector<size_t> HiCacheWrites::source_retained_nodes(size_t source_fact) const {
    const auto & site = sources_.at(source_fact);
    std::vector<size_t> nodes;
    nodes.reserve(site.transfers.size() + 1);
    for (const auto & [node, bytes] : site.transfers) nodes.push_back(node);
    nodes.push_back(site.completion);
    return nodes;
}

void HiCacheWrites::register_write(const HiCacheFact & fact, const model::HiCacheDeviceWrite & write) {
    const auto pending = state_.pending_device_writes(fact);
    const auto found = std::ranges::find_if(pending, [&](const auto & item) { return item.header.operation_id == write.header.operation_id; });
    if (found == pending.end() || found->header.cache_scope != write.header.cache_scope || found->schedule.resource_lane != write.schedule.resource_lane
        || found->schedule.effective_byte_count != write.schedule.effective_byte_count)
        throw std::logic_error("D2H execution requires the pending target operation and its payload/resource work");
    const auto first =
        std::ranges::find_if(pending, [&](const auto & item) { return !submitted_.contains({ item.header.cache_scope, item.header.operation_id }); });
    if (first == pending.end() || first->header.operation_id != write.header.operation_id)
        throw std::logic_error("D2H submissions must preserve the target controller queue order: operation=" + write.header.operation_id
                               + " owner=" + std::to_string(write.header.source_node_id) + " epoch=" + std::to_string(write.header.enqueue_epoch)
                               + " first=" + (first == pending.end() ? "none" : first->header.operation_id)
                               + " first_owner=" + (first == pending.end() ? "none" : std::to_string(first->header.source_node_id))
                               + " first_epoch=" + (first == pending.end() ? "none" : std::to_string(first->header.enqueue_epoch)));
    if (!submitted_.emplace(write.header.cache_scope, write.header.operation_id).second) throw std::logic_error("Target write was already submitted");
}

size_t HiCacheWrites::complete_after(const HiCacheFact & fact, const model::HiCacheDeviceWrite & write, size_t last, simulation::FutureDag & future) {
    const auto done =
        future.append({ .name = "target D2H completion", .category = "hicache_patch", .is_cpu = false, .lane_key = write.schedule.resource_lane });
    future.depend(last, done);
    completions_.emplace(done, Completion{ fact, write.header.operation_id });
    lane_tail_[write.schedule.resource_lane] = done;
    return done;
}

size_t HiCacheWrites::submit_expanded(const HiCacheFact & fact, const model::HiCacheDeviceWrite & write, size_t write_start, size_t completion,
                                      simulation::FutureDag & future) {
    register_write(fact, write);
    if (const auto previous = lane_tail_.find(write.schedule.resource_lane); previous != lane_tail_.end()) future.depend(previous->second, write_start);
    return complete_after(fact, write, completion, future);
}

void HiCacheWrites::advance(size_t node, uint64_t time) {
    const auto found = completions_.find(node);
    if (found == completions_.end()) return;
    auto fact = found->second.fact;
    fact.ts = time;
    fact.execution_anchor_node_id = node;
    state_.complete_device_write(fact, found->second.operation);
    completions_.erase(found);
    ++completed_;
}

} // namespace markov::trace_graph::modules::hicache::runtime
