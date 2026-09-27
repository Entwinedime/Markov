#include "markov/trace_graph/modules/hicache/runtime/load_execution.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_expansion.hpp"
#include <set>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::runtime {

std::vector<size_t> HiCacheHostExpansion::resource_nodes() const {
    std::vector<size_t> result;
    result.reserve(streams.size() + waits.size());
    for (const auto & stream : streams) result.push_back(stream.source_node);
    for (const auto & wait : waits) result.push_back(wait.source_node);
    return result;
}

std::vector<size_t> HiCacheHostExpansion::resource_lanes(const core::DagGraph & graph) const {
    auto result = resource_nodes();
    for (auto & node : result) node = graph.node(node).lane_id;
    return result;
}

std::optional<size_t> find_host_worker_resource(const core::DagGraph & graph, const simulation::detail::CpuTaskQueues & queues, size_t main_node,
                                                std::string_view submission_name) {
    const auto & main = graph.node(main_node);
    std::optional<size_t> found;
    for (const auto & task : queues.tasks) {
        const auto & submission = graph.node(task.submission);
        const auto & worker = graph.node(task.first);
        if (!submission.active || !worker.active || submission.lane_id != main.lane_id || submission.gpu_id != main.gpu_id || worker.gpu_id != main.gpu_id
            || graph.event_for_node(task.submission).name != submission_name)
            continue;
        if (found && graph.node(*found).lane_id != worker.lane_id)
            throw std::runtime_error("Host submission identifies multiple worker lanes: " + std::string(submission_name));
        found = task.first;
    }
    return found;
}

std::map<std::string, size_t> host_resource_roles(const core::DagGraph & graph, const HiCacheHostExpansion & plan, size_t main_node,
                                                  std::optional<size_t> compute_node) {
    const auto main_lane = graph.node_lane_key(main_node);
    if (!graph.node(main_node).is_cpu || plan.nodes.at(plan.host_return).work.lane_key != main_lane)
        throw std::runtime_error("Host resource roles require a matching CPU main lane");
    std::map<std::string, size_t> roles{
        { "main", main_node }
    };
    if (compute_node) {
        if (graph.node(*compute_node).is_cpu || graph.node(*compute_node).gpu_id != graph.node(main_node).gpu_id)
            throw std::runtime_error("Host compute resource must be a same-rank device stream");
        roles.emplace("compute", *compute_node);
    }
    const auto require_compute = [&](std::string_view lane) {
        if (!compute_node || graph.node_lane_key(*compute_node) != lane)
            throw std::runtime_error("Host device work lacks an explicit matching compute resource");
    };
    for (const auto node : plan.resource_nodes()) require_compute(graph.node_lane_key(node));
    std::map<std::string, std::set<std::string>> submissions;
    std::map<std::string, size_t> workers;
    for (const auto & node : plan.nodes) {
        if (!node.work.is_cpu) {
            require_compute(node.work.lane_key);
            continue;
        }
        if (!node.queue_member) {
            if (node.work.lane_key != main_lane) throw std::runtime_error("Host work has an unidentified CPU lane");
            continue;
        }
        if (!node.submission) throw std::runtime_error("Host worker lacks a submission");
        bool found = false;
        for (const auto & [original, local] : plan.source_nodes) {
            if (local != *node.submission) continue;
            if (graph.node_lane_key(original) != main_lane) throw std::runtime_error("Host worker submission is not on the main lane");
            submissions[node.work.lane_key].insert(graph.event_for_node(original).name);
            found = true;
        }
        if (!found) throw std::runtime_error("Host worker submission has no original call identity");
        workers.emplace(node.work.lane_key, *node.queue_member);
    }
    for (const auto & [lane, names] : submissions) {
        std::string role = "worker";
        for (const auto & name : names) role += "|" + name;
        if (!roles.emplace(role, workers.at(lane)).second) throw std::runtime_error("Host submission role names multiple worker lanes");
    }
    return roles;
}

std::map<std::string, size_t> write_resource_roles(const core::DagGraph & graph, const HiCacheWriteExpansion & plan, size_t compute_record) {
    std::map<std::string, size_t> roles;
    const auto add = [&](const std::string & role, size_t node) {
        const auto [at, inserted] = roles.emplace(role, node);
        if (!inserted && graph.node(at->second).lane_id != graph.node(node).lane_id)
            throw std::runtime_error("Write resource role names multiple lanes: " + role);
    };
    if (graph.node(compute_record).is_cpu || graph.event_for_node(compute_record).name != "EVENT_RECORD")
        throw std::runtime_error("Write compute readiness lacks its device Record");
    add("compute_ready", compute_record);
    const auto & main_lane = plan.nodes.at(plan.host_return).work.lane_key;
    std::map<std::string, std::set<std::string>> worker_submissions;
    std::map<std::string, size_t> workers;
    for (const auto & node : plan.nodes) {
        if (!node.queue_member) continue;
        if (!node.submission) throw std::runtime_error("Write worker has no local submission");
        worker_submissions[node.work.lane_key].insert(plan.nodes.at(*node.submission).work.name);
        workers.emplace(node.work.lane_key, *node.queue_member);
    }
    for (const auto & [lane, names] : worker_submissions) {
        std::string role = "worker";
        for (const auto & name : names) role += "|" + name;
        add(role, workers.at(lane));
    }
    for (const auto & [original, local] : plan.source_nodes) {
        if (plan.nodes.at(local).work.lane_key == main_lane) add("main", original);
        if (plan.payload.contains(local)) {
            if (graph.node(original).is_cpu || graph.event_for_node(original).arg("operation") != "device to host")
                throw std::runtime_error("Write payload resource is not observed D2H");
            add("payload", original);
        }
    }
    if (!roles.contains("main") || !roles.contains("payload")) throw std::runtime_error("Write resource roles lack main or payload lane");
    std::set<std::string> covered;
    for (const auto & [role, node] : roles) covered.insert(std::string(graph.node_lane_key(node)));
    for (const auto & node : plan.nodes)
        if (!covered.contains(node.work.lane_key)) throw std::runtime_error("Write template has an unidentified resource lane");
    return roles;
}

std::optional<LoadIndexResources> observe_load_index_resources(const patch::HiCacheSourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues,
                                                               size_t main_node, size_t compute_node) {
    const auto & graph = source.graph();
    if (!graph.node(main_node).is_cpu || graph.node(compute_node).is_cpu || graph.node(main_node).gpu_id != graph.node(compute_node).gpu_id)
        throw std::invalid_argument("Load resources require a same-rank main thread and compute stream");
    std::optional<size_t> worker;
    for (size_t task_id = 0; task_id < queues.tasks.size(); ++task_id) {
        const auto & task = queues.tasks[task_id];
        if (!graph.node(task.submission).active || !graph.node(task.first).active || graph.node(task.submission).lane_id != graph.node(main_node).lane_id
            || graph.node(task.first).gpu_id != graph.node(main_node).gpu_id || !graph.event_for_node(task.submission).name.starts_with("Enqueue@"))
            continue;
        std::vector<size_t> pending{ task.first };
        std::set<size_t> visited;
        for (size_t i = 0; i < pending.size(); ++i) {
            const auto node = pending[i];
            if (!visited.insert(node).second) continue;
            for (const auto id : source.outgoing_edge_ids(node)) {
                const auto & edge = graph.edge(id);
                const auto & next = graph.node(edge.dst);
                if (!edge.active || !next.active) continue;
                if (next.is_cpu) {
                    if (queues.node_task.at(edge.dst) == task_id) pending.push_back(edge.dst);
                    continue;
                }
                if (next.lane_id != graph.node(compute_node).lane_id) continue;
                if (worker && graph.node(*worker).lane_id != graph.node(task.first).lane_id)
                    throw std::runtime_error("Compute submissions identify multiple worker FIFOs");
                worker = task.first;
            }
        }
    }
    if (!worker) return std::nullopt;
    return LoadIndexResources{ *worker, compute_node };
}

} // namespace markov::trace_graph::modules::hicache::runtime
