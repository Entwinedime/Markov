#include "markov/trace_graph/structure/hicache/resources.hpp"
#include "markov/trace_graph/frontend/model_config.hpp"
#include "markov/trace_graph/modules/hicache/layer_waits.hpp"
#include "markov/trace_graph/modules/hicache/patch/layer_wait_insertion.hpp"
#include "markov/trace_graph/source_dag/call_regions.hpp"

#include <stdexcept>
#include <vector>

namespace markov::trace_graph::structure::hicache {

std::string io_resource_lane(const frontend::HiCacheIoResourceLanesConfig & lanes, const std::string & kind, const std::string & scope) {
    const auto local = kind == "prefetch"                ? "host_storage_read_lane"
                       : kind == "write_host_to_storage" ? "host_storage_write_lane"
                       : kind == "load"                  ? "host_to_device_lane"
                                                         : "device_to_host_lane";
    const bool shared = (kind == "prefetch" && lanes.shared_storage_read) || (kind == "write_host_to_storage" && lanes.shared_storage_write);
    return shared ? local : scope + "/" + local;
}

SchedulerDeviceResources observe_scheduler_compute_resources(const modules::hicache::patch::HiCacheSourceDagIndex & source) {
    const auto & graph = source.graph();
    const auto waits = modules::hicache::observe_hicache_layer_waits(source);
    SchedulerDeviceResources resources;
    const auto witness = [&](SchedulerComputeResources & roles, size_t main, size_t compute) {
        const auto & event = graph.event_for_node(main);
        const auto [at, inserted] = roles.emplace(std::pair{ event.pid, event.tid }, compute);
        if (!inserted && graph.node(at->second).lane_id != graph.node(compute).lane_id)
            throw std::runtime_error("Source calls identify conflicting streams within one execution role: " + event.pid + "/" + event.tid);
    };

    std::vector<const modules::hicache::HiCacheLayerWaitCall *> inactive;
    for (const auto & call : waits.calls) {
        if (!call.issue.empty()) continue;
        if (call.enabled) witness(resources.forward, *call.submission, *call.device_wait);
        else inactive.push_back(&call);
    }
    const auto positions = modules::hicache::patch::observe_inactive_layer_positions(source, waits, inactive);
    for (const auto * call : inactive) {
        const auto & position = positions.at(call);
        if (position.issue.empty()) witness(resources.forward, *call->before, position.after);
    }

    // The scheduler can enqueue allocation and cache control on a different
    // stream from model forward, even on the same CPU thread. Owned allocator
    // submissions establish that role without borrowing a cost/template or
    // mistaking an external synchronization producer for the allocator stream.
    const auto queues = simulation::detail::discover_cpu_task_queues(graph);
    for (const auto & region : source_dag::observe_allocator_regions(source, queues)) {
        if (region.work.caller.owned_node_ids.empty()) continue;
        const auto main = region.work.caller.owned_node_ids.front();
        for (const auto node : region.work.submitted_nodes)
            if (graph.node(node).active && !graph.node(node).is_cpu) witness(resources.schedule, main, node);
    }
    return resources;
}

} // namespace markov::trace_graph::structure::hicache
