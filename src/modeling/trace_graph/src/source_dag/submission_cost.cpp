#include "markov/trace_graph/source_dag/submission_cost.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/source_dag/tensor_cost.hpp"

#include <algorithm>
#include <array>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace markov::trace_graph::source_dag {
namespace patch = modules::hicache::patch;
using modules::hicache::runtime::HiCacheHostExpansion;
using modules::hicache::runtime::HiCacheWriteExpansion;

namespace {

std::vector<HostOperationMeasurement> observe_caller_costs(const HiCacheHostExpansion & plan, const patch::HiCacheSourceDagIndex & source) {
    std::vector<HostOperationMeasurement> costs(plan.nodes.size());
    for (size_t i = 0; i < plan.nodes.size(); ++i) {
        const auto & node = plan.nodes[i];
        if (node.work.is_cpu && !node.queue_member) {
            costs[i].main_us = 0;
            costs[i].main_residual_us = node.work.cpu_gap_after;
        }
    }

    // Synthetic gap/return boundaries have no CPU service. Original leaves
    // must be measurable independently; replay-normalized waits are not data.
    for (const auto & [original, local] : plan.source_nodes) {
        const auto & node = plan.nodes[local];
        if (!node.work.is_cpu || node.queue_member) continue;
        const auto measured = observe_nonblocking_cpu_service(source, original);
        costs[local].main_us = measured;
    }
    return costs;
}

constexpr auto cost_fields = { &HostOperationMeasurement::main_us,          &HostOperationMeasurement::main_residual_us,
                               &HostOperationMeasurement::worker_us,        &HostOperationMeasurement::worker_residual_us,
                               &HostOperationMeasurement::worker_return_us, &HostOperationMeasurement::worker_return_residual_us,
                               &HostOperationMeasurement::dispatch_us };
HostOperationMeasurement mean_cost(std::span<const HostOperationMeasurement> samples) {
    HostOperationMeasurement result;
    for (const auto field : cost_fields) {
        uint64_t total = 0;
        if (std::ranges::any_of(samples, [&](const auto & sample) { return !(sample.*field); })) continue;
        for (const auto & sample : samples) total = core::checked_add_u64(total, *(sample.*field), "Load operation cost overflow");
        result.*field = total / samples.size() + (total % samples.size() >= (samples.size() + 1) / 2);
    }

    if (std::ranges::all_of(samples, [](const auto & sample) { return sample.device_us.has_value(); })) {
        double total = 0;
        for (const auto & sample : samples) total += *sample.device_us;
        result.device_us = total / samples.size();
    }
    return result;
}

void add_measurement(std::optional<uint64_t> & total, std::optional<uint64_t> value) {
    if (total && value) *total = core::checked_add_u64(*total, *value, "Measured service sum overflow");
    else total.reset();
}

void observe_worker_cost(const HiCacheHostExpansion & plan, const patch::HiCacheSourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues,
                         size_t submission, HostOperationMeasurement & cost) {
    std::vector<size_t> workers;
    std::set<size_t> devices;
    std::optional<size_t> first;
    for (const auto & [original, local] : plan.source_nodes) {
        const auto & node = plan.nodes[local];
        if (node.submission != submission) continue;
        workers.push_back(original);
        if (!node.task_predecessor) {
            if (first) return;
            first = original;
        }
        for (const auto edge_id : source.outgoing_edge_ids(original)) {
            const auto & edge = source.graph().edge(edge_id);
            if (edge.active && edge.kind == core::DagEdgeKind::Correlation && !source.graph().node(edge.dst).is_cpu) devices.insert(edge.dst);
        }
    }
    if (!first || devices.size() != 1) return;

    const auto & graph = source.graph();
    const auto & task = queues.tasks.at(queues.node_task.at(*first));
    cost.dispatch_us = graph.cpu_service_ready_delay(*first, task.ready_delay_us);
    if (const auto measured = observe_worker_launch(source, queues, std::move(workers), devices)) {
        cost.worker_us = measured->launch_us;
        cost.worker_residual_us = measured->launch_residual_us;
        cost.worker_return_us = measured->return_us;
        cost.worker_return_residual_us = measured->return_residual_us;
    }

    const auto device = *devices.begin();
    const auto & event = graph.event_for_node(device);
    if (event.name == "EVENT_WAIT") {
        cost.device_us = observe_device_event_wait_service(source, device);
        return;
    }
    if (graph.node(device).kind != core::DagNodeKind::TraceEvent || graph.node(device).duration != event.dur) return;
    for (const auto edge_id : source.incoming_edge_ids(device)) {
        const auto & edge = graph.edge(edge_id);
        if (edge.active && edge.kind == core::DagEdgeKind::Sync) return;
    }
    cost.device_us = static_cast<double>(event.dur) + static_cast<double>(event.dur_submicro_ns) / 1000.0;
}

} // namespace

void append_host_operation_observations(nlohmann::json & rows, const structure::OperationWork & work, std::string_view position,
                                        const HostOperationMeasurement & measured, bool corrected, bool submitted) {
    const auto append = [&](std::string_view scope, std::optional<double> duration_us) {
        if (!duration_us) return;

        rows.push_back({
            {       "position",            position },
            {      "operation",      work.operation },
            { "implementation", work.implementation },
            {       "features",       work.features },
            {    "duration_us",        *duration_us },
            {   "timing_scope",               scope }
        });
    };
    using Measurement = HostOperationMeasurement;
    const std::array fields{
        std::pair{                     "cpu_service",                   &Measurement::main_us },
        std::pair{               "residual_interval",          &Measurement::main_residual_us },
        std::pair{           "worker_launch_service",                 &Measurement::worker_us },
        std::pair{ "worker_launch_residual_interval",        &Measurement::worker_residual_us },
        std::pair{           "worker_return_service",          &Measurement::worker_return_us },
        std::pair{ "worker_return_residual_interval", &Measurement::worker_return_residual_us },
        std::pair{               "dispatch_interval",               &Measurement::dispatch_us },
    };
    for (size_t index = 0; index < (submitted ? fields.size() : 2); ++index) {
        const auto & [scope, member] = fields[index];
        std::string label = scope;
        if (!corrected && index != fields.size() - 1) {
            label = "profiled_" + label;
            if (index == 0) label = "profiled_cpu";
        }

        append(label, measured.*member);
    }
    if (submitted) append("device_service", measured.device_us);
}

std::optional<nlohmann::json> load_index_operator_observations(const HiCacheHostExpansion & plan, const patch::HiCacheSourceDagIndex & source,
                                                               const simulation::detail::CpuTaskQueues & queues, uint64_t tokens, uint64_t page_size,
                                                               std::span<const uint64_t> clone_lengths, std::string_view position, bool corrected) {
    if (!page_size || !tokens || tokens % page_size || plan.streams.size() != 1 || !plan.waits.empty() || !plan.event_waits.empty()) return std::nullopt;

    const auto & nodes = plan.nodes;
    std::vector<size_t> submissions;
    for (size_t index = 0; index < nodes.size(); ++index)
        if (nodes[index].work.is_cpu && !nodes[index].queue_member && nodes[index].work.name.starts_with("Enqueue@")) submissions.push_back(index);

    const std::array allocation_calls{ "Enqueue@aclnnMuls", "Enqueue@aclnnArange", "Enqueue@aclnnAdd" };
    if (submissions.size() <= allocation_calls.size()) return std::nullopt;
    for (size_t index = 0; index < allocation_calls.size(); ++index)
        if (nodes[submissions[index]].work.name != allocation_calls[index]) return std::nullopt;
    for (size_t index = allocation_calls.size(); index < submissions.size(); ++index)
        if (nodes[submissions[index]].work.name != "Enqueue@aclnnInplaceCopy") return std::nullopt;

    std::vector<uint64_t> lengths(clone_lengths.begin(), clone_lengths.end());
    if (lengths.empty() && submissions.size() == allocation_calls.size() + 1) lengths.push_back(tokens);
    if (lengths.size() != submissions.size() - allocation_calls.size()) return std::nullopt;
    uint64_t total = 0;
    for (const auto length : lengths) {
        if (!length || length % page_size) return std::nullopt;
        total = core::checked_add_u64(total, length, "Measured clone lengths overflow");
    }
    if (total != tokens) return std::nullopt;

    auto rows = nlohmann::json::array();
    std::set<size_t> assigned;
    size_t previous = 0;
    const auto segment = [&](size_t last) {
        HostOperationMeasurement measured{ .main_us = 0, .main_residual_us = 0 };
        for (size_t index = previous; index <= last; ++index) {
            const auto & node = nodes[index];
            if (!node.work.is_cpu || node.queue_member) continue;
            add_measurement(measured.main_us, node.work.duration);
            add_measurement(measured.main_residual_us, node.work.cpu_gap_after);
        }
        previous = last + 1;
        return measured;
    };
    const std::array names{ "multiply", "range", "add" };
    for (size_t index = 0; index < submissions.size(); ++index) {
        const auto submission = submissions[index];
        std::set<size_t> devices;
        size_t heads = 0;
        for (size_t local = 0; local < nodes.size(); ++local) {
            if (nodes[local].submission != submission) continue;
            heads += !nodes[local].task_predecessor;
            for (const auto & edge : plan.edges)
                if (edge.from == local && !nodes[edge.to].work.is_cpu) devices.insert(edge.to);
        }
        if (heads != 1 || devices.size() != 1 || !assigned.insert(*devices.begin()).second) return std::nullopt;

        auto measured = segment(submission);
        observe_worker_cost(plan, source, queues, submission, measured);
        if (!measured.worker_us || !measured.worker_residual_us || !measured.worker_return_us || !measured.worker_return_residual_us || !measured.dispatch_us
            || !measured.device_us)
            return std::nullopt;

        const std::string name = index < names.size() ? names[index] : "clone";
        const auto length = index < names.size() ? tokens : lengths[index - names.size()];
        structure::OperationWork work{
            .operation = "load.index." + name,
            .implementation = "ascend.paged_unsorted",
            .features = { { "requested_tokens", static_cast<double>(length) },
                         { "page_size_tokens", static_cast<double>(page_size) },
                         { "pages", static_cast<double>(length / page_size) } }
        };
        if (name == "clone") work.features["first_clone"] = index == names.size() ? 1.0 : 0.0;
        append_host_operation_observations(rows, work, std::string(position) + ":" + name + ":" + std::to_string(index), measured, corrected, true);
    }
    for (size_t index = 0; index < nodes.size(); ++index)
        if (!nodes[index].work.is_cpu && !assigned.contains(index)) return std::nullopt;

    const structure::OperationWork tail{
        .operation = "load.index.tail",
        .implementation = "ascend.paged_unsorted",
        .features = { { "requested_tokens", static_cast<double>(tokens) },
                     { "page_size_tokens", static_cast<double>(page_size) },
                     { "pages", static_cast<double>(tokens / page_size) } }
    };
    append_host_operation_observations(rows, tail, std::string(position) + ":tail", segment(nodes.size() - 1), corrected, false);
    return rows;
}

std::optional<PagedReleaseMeasurement> observe_paged_release_cost(const HiCacheHostExpansion & plan, const patch::HiCacheSourceDagIndex & source,
                                                                  const simulation::detail::CpuTaskQueues & queues, bool paged) {
    if (plan.waits.size() != (paged ? 2 : 0) || plan.streams.size() != 1 || !plan.event_waits.empty()) return std::nullopt;
    const auto & nodes = plan.nodes;
    std::optional<size_t> submit, worker, device;
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (!nodes[i].work.is_cpu) {
            if (device) return std::nullopt;
            device = i;
        }
        else if (nodes[i].queue_member) {
            if (!nodes[i].submission || (submit && submit != nodes[i].submission)) return std::nullopt;
            submit = nodes[i].submission;
            if (!nodes[i].task_predecessor) {
                if (worker) return std::nullopt;
                worker = i;
            }
        }
    }
    if (!worker || !device || !nodes[*submit].work.name.ends_with("Enqueue@aclnnCat")) return std::nullopt;
    if (*submit >= plan.host_return || plan.streams[0].first != *device || plan.streams[0].last != *device) return std::nullopt;

    const auto caller = observe_caller_costs(plan, source);
    size_t previous = 0;
    const auto segment = [&](size_t last) {
        HostOperationMeasurement cost{ .main_us = 0, .main_residual_us = 0 };
        for (size_t i = previous; i <= last; ++i) {
            if (!nodes[i].work.is_cpu || nodes[i].queue_member) continue;
            add_measurement(cost.main_us, caller[i].main_us);
            add_measurement(cost.main_residual_us, caller[i].main_residual_us);
        }
        previous = last + 1;
        return cost;
    };
    PagedReleaseMeasurement cost;
    if (paged) {
        auto first = plan.waits[0].consumer, second = plan.waits[1].consumer;
        if (second < first) std::swap(first, second);
        if (!(first < second && second < *submit)) return std::nullopt;
        // These source predecessors can have different node identities on
        // the same compute stream. Resource roles are validated by the
        // exporter before partitioning; do not compare trace-local node IDs.
        if (!nodes[first].work.name.ends_with("AscendCL@aclrtSynchronizeStream") || !nodes[second].work.name.ends_with("AscendCL@aclrtSynchronizeStream"))
            return std::nullopt;
        // Keep each blocking API outside its preparation interval. Service
        // is supplied by the independent idle primitive; the observed gap
        // after its return is a separate residual, not device waiting time.
        cost.before_first_sync = first ? segment(first - 1) : HostOperationMeasurement{ .main_us = 0, .main_residual_us = 0 };
        cost.first_sync = caller[first];
        previous = first + 1;
        cost.before_second_sync = second > previous ? segment(second - 1) : HostOperationMeasurement{ .main_us = 0, .main_residual_us = 0 };
        cost.second_sync = caller[second];
        previous = second + 1;
    }
    cost.concatenate = segment(*submit);
    cost.tail = segment(plan.host_return);
    observe_worker_cost(plan, source, queues, *submit, cost.concatenate);
    return cost;
}

std::optional<PageFirstSubmissionMeasurement> observe_page_first_submission_cost(const HiCacheWriteExpansion & plan,
                                                                                 const patch::HiCacheSourceDagIndex & source,
                                                                                 const simulation::detail::CpuTaskQueues & queues, size_t completion_count) {
    if (!plan.event_waits.empty() || plan.waits.size() != 1 || !completion_count) return std::nullopt;
    const auto & nodes = plan.nodes;
    std::vector<size_t> records, waits, copies;
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto & node = nodes[i];
        if (!node.work.is_cpu || node.queue_member) continue;
        if (node.work.name.ends_with("Enqueue@record_event")) records.push_back(i);
        if (node.work.name.ends_with("Enqueue@wait_event")) waits.push_back(i);
        if (node.work.name.ends_with("AscendCL@aclrtMemcpy2dAsync")) copies.push_back(i);
    }
    const auto sync = plan.waits.front().consumer;
    // The shared idle primitive measures stream synchronization, not an
    // arbitrary event wait or a different blocking API.
    if (!nodes[sync].work.name.ends_with("AscendCL@aclrtSynchronizeStream")) return std::nullopt;
    if (waits.size() != 1 || records.size() != completion_count + 1 || copies.empty() || copies.size() % 2) return std::nullopt;
    // A page consists of equal K/V payloads. Do not label other layouts as
    // page-first observations merely because their host calls look similar.
    if (plan.payload.size() != copies.size() || !plan.payload.begin()->second
        || std::ranges::any_of(plan.payload, [&](const auto & payload) { return payload.second != plan.payload.begin()->second; }))
        return std::nullopt;
    if (!(sync < records[0] && records[0] < waits[0] && waits[0] < copies.front() && copies.back() < records[1])) return std::nullopt;

    const auto caller = observe_caller_costs(plan, source);
    size_t previous = 0;
    const auto segment = [&](size_t end, bool submitted = false) {
        HostOperationMeasurement cost{ .main_us = 0, .main_residual_us = 0 };
        for (size_t i = previous; i <= end; ++i) {
            if (!nodes[i].work.is_cpu || nodes[i].queue_member) continue;
            add_measurement(cost.main_us, caller[i].main_us);
            add_measurement(cost.main_residual_us, caller[i].main_residual_us);
        }
        previous = end + 1;
        if (!submitted) return cost;

        observe_worker_cost(plan, source, queues, end, cost);
        return cost;
    };
    PageFirstSubmissionMeasurement cost;
    // Keep blocking synchronization out of the preparation measurement. Its
    // return-to-next-call gap remains on the synchronization slot, after the
    // dependency wait. An empty prefix is genuinely no work, not missing data.
    cost.prepare_sync = sync ? segment(sync - 1) : HostOperationMeasurement{ .main_us = 0, .main_residual_us = 0 };
    cost.index_sync = caller[sync];
    previous = sync + 1;
    cost.start_record = segment(records[0], true);
    cost.wait_event = segment(waits[0], true);
    cost.first_copy = segment(copies.front());
    std::vector<HostOperationMeasurement> repeated;
    for (size_t i = 1; i < copies.size(); ++i) repeated.push_back(segment(copies[i]));
    cost.copy = mean_cost(repeated);
    cost.first_completion_record = segment(records[1], true);
    repeated.clear();
    for (size_t i = 2; i < records.size(); ++i) repeated.push_back(segment(records[i], true));
    if (!repeated.empty()) cost.completion_record = mean_cost(repeated);
    cost.tail = segment(plan.host_return);
    return cost;
}

} // namespace markov::trace_graph::source_dag
