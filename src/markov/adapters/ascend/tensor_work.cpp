#include "markov/adapters/ascend/tensor_work.hpp"

#include "markov/adapters/ascend/trace.hpp"
#include "markov/trace_graph/source/chrome_reader.hpp"
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
            regions.push_back({ marker.resource, marker.begin_ns, marker.end_ns, marker.record });
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
    const auto services = bind_device_services(observed, graph, regions);
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
    }
    return {
        {           "status",                                                                                  "completed"                             },
        {          "capture",                                                            std::filesystem::absolute(capture).lexically_normal().string() },
        {            "trace",                                                                                                            trace.string() },
        {         "metadata",                                                                                                                  metadata },
        {     "measurements",                                                                                                   std::move(measurements) },
        { "unresolved_flows",                                                                                          observed.unresolved_flows.size() },
        {    "qualification",
         "Independent initialized device services from profiled exact native flows; caller and worker timestamps are provenance, not normal CPU costs." }
    };
}

} // namespace markov::adapters::ascend
