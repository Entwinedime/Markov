#include "markov/adapters/ascend/tensor_work.hpp"

#include "markov/adapters/ascend/trace.hpp"
#include "markov/trace_graph/source/chrome_reader.hpp"
#include "markov/trace_graph/source/native_queues.hpp"
#include "markov/trace_graph/source/services.hpp"

#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace markov::adapters::ascend {

nlohmann::json tensor_work_report(const std::filesystem::path & capture) {
    using namespace trace_graph::source;
    using Json = nlohmann::json;
    std::ifstream input(capture);
    const auto metadata = Json::parse(input);
    if (metadata.at("status") != "completed" || !metadata.at("trace").is_string() || metadata.at("native_observer_requested") != false)
        throw std::runtime_error("Tensor device measurements require a completed independent trace without the native observer");
    auto trace = std::filesystem::path(metadata.at("trace").get<std::string>());
    if (trace.is_relative()) trace = capture.parent_path() / trace;
    trace = std::filesystem::absolute(trace).lexically_normal();
    const ProfileFile file{ Channel::Torch, trace, std::filesystem::file_size(trace) };
    Observations observed;
    RuntimeCausality causality;
    TraceReader reader(observed, causality, file, 0);
    read_chrome_trace(trace, reader);
    reader.finish();
    observed.normalize_resources();
    observed.resolve_flows();
    causality.connect(observed);

    const auto & recipe = metadata.at("recipe");
    const auto repeats = recipe.at("repeats").get<std::size_t>();
    const auto & cases = recipe.at("cases");
    if (!cases.is_array() || cases.empty() || repeats == 0) throw std::runtime_error("Empty tensor measurement recipe");
    std::map<std::string, Index> markers;
    for (Index id = 0; id < observed.spans.size(); ++id) {
        const auto & name = observed.names[observed.spans[id].name];
        if (name.starts_with("markov.tensor:") && !markers.emplace(name, id).second) throw std::runtime_error("Repeated tensor trace marker");
    }
    if (markers.size() != cases.size() * repeats) throw std::runtime_error("Tensor trace marker count differs from its recipe");

    std::vector<ObservedRegion> regions;
    std::vector<ObservedRegion> caller_apis;
    std::vector<Index> queue_ids;
    std::vector<trace_graph::planning::NativeCall> definitions;
    auto measurements = Json::array();
    for (Index case_id = 0; case_id < cases.size(); ++case_id) {
        const auto & operation = cases[case_id].at("operation");
        if ((operation != "clone" && operation != "concatenate") || (cases[case_id].at("dtype") != "int32" && cases[case_id].at("dtype") != "int64"))
            throw std::runtime_error("Unsupported tensor measurement operation or dtype");
        for (Index ordinal = 0; ordinal < repeats; ++ordinal) {
            const auto & marker = observed.spans.at(markers.at("markov.tensor:" + std::to_string(case_id) + ":" + std::to_string(ordinal)));
            const NativeQueueCall * queue = nullptr;
            for (const auto & call : observed.native_queue_calls) {
                if (call.submission.resource != marker.resource || call.submission.begin_ns < marker.begin_ns || marker.end_ns < call.submission.end_ns)
                    continue;
                if (queue) throw std::runtime_error("Tensor measurement contains more than one native submission");
                queue = &call;
            }
            const auto expected = operation == "clone" ? "aclnnInplaceCopy" : "aclnnCat";
            if (!queue || observed.names[queue->name] != expected) throw std::runtime_error("Tensor trace does not contain its declared queued operation");
            const ObservedSpan * caller_api = nullptr;
            for (const auto & span : observed.spans) {
                if (span.resource != marker.resource || span.begin_ns < marker.begin_ns || marker.end_ns < span.end_ns
                    || span.begin_ns > queue->submission.begin_ns || span.end_ns < queue->submission.end_ns
                    || observed.names[span.name] != (operation == "clone" ? "aten::clone" : "aten::cat"))
                    continue;
                if (caller_api) throw std::runtime_error("Tensor trace contains ambiguous caller API boundaries");
                caller_api = &span;
            }
            if (!caller_api) throw std::runtime_error("Tensor trace lacks its physical caller API");
            regions.push_back({ marker.resource, marker.begin_ns, marker.end_ns, marker.record });
            caller_apis.push_back({ caller_api->resource, caller_api->begin_ns, caller_api->end_ns, caller_api->record });
            queue_ids.push_back(static_cast<Index>(queue - observed.native_queue_calls.data()));
            double elements = 0;
            for (const auto & size : cases[case_id].at("input_sizes")) elements += size.get<double>();
            const auto dtype = cases[case_id].at("dtype").get<std::string>();
            trace_graph::planning::Work work{
                { operation == "clone" ? "tensor.clone" : "tensor.concatenate",
                 std::string(operation == "clone" ? "aten.clone:npu:" : "aten.cat:npu:") + dtype,
                 "device_service" },
                { { "elements", elements },
                 { "element_bytes", dtype == "int64" ? 8 : 4 },
                 { "tensor_rank", 1 },
                 { "input_tensors", cases[case_id].at("input_sizes").size() } }
            };
            definitions.push_back({ work, trace_graph::planning::CallKind::Queued, { work } });
            measurements.push_back({
                {                "case",                          case_id },
                {             "ordinal",                          ordinal },
                {            "position", queue->execution.record.position },
                {     "marker_position",           marker.record.position },
                { "submission_begin_ns",       queue->submission.begin_ns },
                {   "submission_end_ns",         queue->submission.end_ns },
                {     "worker_begin_ns",        queue->execution.begin_ns },
                {       "worker_end_ns",          queue->execution.end_ns }
            });
        }
    }

    // Use exact flow destinations and exclusive services, never kernel order or proximity.
    const auto graph = build_source_graph(observed);
    const NativeQueues queues(observed, graph);
    const auto services = bind_device_services(observed, graph, regions);
    std::vector<ObservedRegion> cpu_regions;
    std::vector<std::pair<Index, std::string>> cpu_owners;
    for (Index id = 0; id < services.size(); ++id) {
        const auto & service = services[id];
        if (!service.available() || !service.resource)
            throw std::runtime_error("Tensor trace lacks a unique exclusive device service at case " + measurements[id].at("case").dump() + ", repeat "
                                     + measurements[id].at("ordinal").dump()
                                     + " (issue=" + (service.issue ? std::to_string(static_cast<int>(*service.issue)) : "none")
                                     + ", service_nodes=" + std::to_string(service.nodes.size()) + ")");
        Time duration = 0;
        auto records = Json::array();
        for (const auto node : service.nodes) {
            duration += graph.graph.nodes()[node].duration_ns;
            records.push_back(observed.spans[graph.node_origins[node].span].record.position);
        }
        if (duration <= 0) throw std::runtime_error("Tensor device service must be positive");
        measurements[id]["device_service_ns"] = duration;
        measurements[id]["device_records"] = std::move(records);
        const auto & queue = observed.native_queue_calls[queue_ids[id]];
        const auto & api = caller_apis[id];
        const auto ports = queues.ports(queue_ids[id], definitions[id]);
        measurements[id]["worker_tid"] =
            observed.resources[queue.execution.resource].identity.substr(observed.resources[queue.execution.resource].identity.rfind(':') + 1);
        measurements[id]["code_preparation"] = ports.code_preparation;
        if (ports.dispatch) measurements[id]["dispatch_ns"] = ports.dispatch->end_ns - ports.dispatch->begin_ns;
        cpu_regions.push_back({ api.resource, api.begin_ns, queue.submission.begin_ns, api.record });
        cpu_owners.emplace_back(id, "caller_before");
        cpu_regions.push_back({ api.resource, queue.submission.begin_ns, api.end_ns, api.record });
        cpu_owners.emplace_back(id, "caller_after");
        if (ports.launches.front().worker_before) {
            cpu_regions.push_back(*ports.launches.front().worker_before);
            cpu_owners.emplace_back(id, "worker_before");
        }
        if (ports.worker_after) {
            cpu_regions.push_back(*ports.worker_after);
            cpu_owners.emplace_back(id, "worker_after");
        }
    }
    const auto cpu = cpu_intervals(observed, graph, cpu_regions);
    for (Index id = 0; id < cpu.size(); ++id) {
        const auto & measured = cpu[id];
        const auto & region = cpu_regions[id];
        const auto & [owner, role] = cpu_owners[id];
        measurements[owner]["profiled_cpu"][role] = {
            {           "begin_ns",             region.begin_ns },
            {             "end_ns",               region.end_ns },
            {           "position",      region.record.position },
            {         "service_ns",         measured.service_ns },
            {        "residual_ns",        measured.residual_ns },
            {            "wait_ns",            measured.wait_ns },
            { "instrumentation_ns", measured.instrumentation_ns },
            {      "unobserved_ns",      measured.unobserved_ns }
        };
    }
    return {
        {           "status",                       "completed"                             },
        {          "capture", std::filesystem::absolute(capture).lexically_normal().string() },
        {            "trace",                                                 trace.string() },
        {         "metadata",                                                       metadata },
        {     "measurements",                                        std::move(measurements) },
        { "unresolved_flows",                               observed.unresolved_flows.size() },
        {    "qualification",
         "Independent device services and profiled caller/worker partitions from shared native ownership. Profiled CPU partitions require separate normal "
         "measurements before estimating normal costs."                                     }
    };
}

} // namespace markov::adapters::ascend
