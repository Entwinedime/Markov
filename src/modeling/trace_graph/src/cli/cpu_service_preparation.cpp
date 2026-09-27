#include "markov/trace_graph/cli/cpu_service_preparation.hpp"
#include "markov/trace_graph/io/cpu_service_input.hpp"
#include "markov/trace_graph/modules/hicache/forward_cpu_service.hpp"
#include "markov/trace_graph/modules/hicache/host_cpu_service.hpp"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace markov::trace_graph::cli {
void prepare_cpu_service(core::DagGraph & graph, const std::string & manifest, const std::string & measurements, const std::string & output) {
    std::ifstream input(measurements);
    if (!input) throw std::runtime_error("Cannot open CPU service measurements");
    const auto data = nlohmann::json::parse(input);
    const auto same_source = [&](const auto & document) {
        const auto source = document.at("source_manifest").template get<std::string>();
        if (source.empty() || std::filesystem::weakly_canonical(source) != std::filesystem::weakly_canonical(manifest))
            throw std::runtime_error("CPU preparation measurements belong to a different source");
    };
    same_source(data);
    same_source(data.at("profiled_steps"));
    same_source(data.at("host_measurements"));
    const auto changes = modules::hicache::plan_forward_cpu_service(graph, data.at("profiled_steps"), data.at("comparison"));
    modules::hicache::attach_forward_cpu_service(graph, changes);
    const auto retained = modules::hicache::attach_host_cpu_service(graph, data.at("host_measurements"));
    auto recorder = nlohmann::json{};
    if (data.contains("recorder_writes")) recorder = modules::hicache::attach_recorder_cpu_service(graph, data.at("recorder_writes"));
    std::ofstream file(output);
    if (!file) throw std::runtime_error("Cannot open CPU service output");
    io::write_cpu_service_cost(file, manifest, graph.cpu_service_cost());

    int64_t retained_cpu = 0;
    for (const auto & row : retained) retained_cpu += row.at("retained_reduction_us").get<int64_t>();
    auto audit_path = std::filesystem::path(output);
    audit_path.replace_extension(".retained.json");
    std::ofstream audit(audit_path);
    if (!audit) throw std::runtime_error("Cannot open retained CPU measurement audit");
    auto result = nlohmann::json{
        {          "source_manifest",        manifest },
        { "retained_recorder_writes", retained.size() },
        { "retained_recorder_cpu_us",    retained_cpu }
    };
#ifdef DEBUG
    result["rows"] = retained;
#endif
    if (!recorder.is_null()) {
#ifdef DEBUG
        for (const auto & row : recorder.at("rows")) result["rows"].push_back(row);
#endif
        result["retained_recorder_writes"] = retained.size() + recorder.at("rows").size();
        result["retained_recorder_cpu_us"] = retained_cpu + recorder.at("retained_cpu_us").get<int64_t>();
        recorder.erase("rows");
        result["source_recorder_correction"] = std::move(recorder);
    }
    audit << result.dump(2) << '\n';
}
} // namespace markov::trace_graph::cli
