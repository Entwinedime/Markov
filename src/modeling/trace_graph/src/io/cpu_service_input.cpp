#include "markov/trace_graph/io/cpu_service_input.hpp"
#include <nlohmann/json.hpp>

namespace markov::trace_graph::io {
void write_cpu_service_cost(std::ostream & output, const std::filesystem::path & source_manifest, const core::CpuServiceCost & cost,
                            const nlohmann::json & reference_io) {
    output << "{\n  \"source_manifest\": " << nlohmann::json(source_manifest.generic_string()).dump();
    // Paired service evidence is provenance for model preparation, not a second
    // correction applied by the CPU simulator.
    if (!reference_io.is_null()) output << ",\n  \"reference_io\": " << reference_io.dump();
    output << ",\n  \"spans\": [";
    bool first = true;
    for (const auto & [lane, intervals] : cost.lanes()) {
        for (const auto & span : intervals) {
            output << (first ? "\n    " : ",\n    ");
            first = false;
            output << nlohmann::json{
                {        "pid",      lane.first },
                {        "tid",     lane.second },
                {   "begin_us",   span.begin_us },
                {     "end_us",     span.end_us },
                { "service_us", span.service_us }
            }.dump();
        }
    }
    output << "\n  ]\n}\n";
    if (!output) throw std::runtime_error("Failed to write CPU service intervals");
}

core::CpuServiceCost read_cpu_service_cost(std::istream & input, const std::filesystem::path & source_manifest) {
    const auto document = nlohmann::json::parse(input);
    const auto source = document.at("source_manifest").get<std::string>();
    if (source.empty() || std::filesystem::weakly_canonical(source) != std::filesystem::weakly_canonical(source_manifest))
        throw std::invalid_argument("CPU service intervals belong to a different source manifest");
    const auto & spans = document.at("spans");
    if (!spans.is_array()) throw std::invalid_argument("CPU service spans must be an array");
    const auto duration = [](const nlohmann::json & value) -> uint64_t {
        if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<int64_t>() < 0))
            throw std::invalid_argument("CPU service coordinates and durations must be non-negative integers");
        return value.get<uint64_t>();
    };
    core::CpuServiceCost result;
    for (const auto & row : spans) {
        auto pid = row.at("pid").get<std::string>(), tid = row.at("tid").get<std::string>();
        if (pid.empty() || tid.empty()) throw std::invalid_argument("CPU service interval lacks a thread identity");
        result.add({ std::move(pid), std::move(tid) }, { duration(row.at("begin_us")), duration(row.at("end_us")), duration(row.at("service_us")) });
    }
    return result;
}
} // namespace markov::trace_graph::io
