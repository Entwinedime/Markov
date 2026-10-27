#include "markov/trace_graph/io/cpu_service_input.hpp"

#include <map>
#include <nlohmann/json.hpp>
#include <utility>
#include <vector>

namespace markov::trace_graph::io {
void write_cpu_service_cost(std::ostream & output, const std::filesystem::path & source_manifest, const core::CpuServiceCost & cost,
                            const std::filesystem::path & light_manifest, const nlohmann::json & reference_io, const nlohmann::json & measurement_window) {
    output << "{\n  \"source_manifest\": " << nlohmann::json(source_manifest.generic_string()).dump();
    output << ",\n  \"light_manifest\": " << nlohmann::json(light_manifest.generic_string()).dump();
    output << ",\n  \"measurement_window\": " << measurement_window.dump();
    // Paired service evidence is provenance for model preparation, not a second
    // correction applied by the CPU simulator.
    if (!reference_io.is_null()) output << ",\n  \"reference_io\": " << reference_io.dump();
    output << ",\n  \"spans\": [";
    bool first = true;
    for (const auto & [lane, intervals] : cost.lanes()) {
        nlohmann::json row{
            {        "pid",    lane.first },
            {        "tid",   lane.second },
            {   "begin_us", uint64_t{ 0 } },
            {     "end_us", uint64_t{ 0 } },
            { "service_us", uint64_t{ 0 } }
        };
        auto & begin = row.at("begin_us");
        auto & end = row.at("end_us");
        auto & service = row.at("service_us");

        // A long capture can contain millions of intervals on the same lane.
        // Reuse the row's keys and thread identity, not its numeric values.
        for (const auto & span : intervals) {
            output << (first ? "\n    " : ",\n    ");
            first = false;
            begin = span.begin_us;
            end = span.end_us;
            service = span.service_us;
            output << row.dump();
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
    std::map<core::CpuServiceCost::Lane, std::vector<core::CpuServiceCost::Span>> intervals;
    for (const auto & row : spans) {
        auto pid = row.at("pid").get<std::string>(), tid = row.at("tid").get<std::string>();
        if (pid.empty() || tid.empty()) throw std::invalid_argument("CPU service interval lacks a thread identity");
        intervals[{ std::move(pid), std::move(tid) }].push_back({ duration(row.at("begin_us")), duration(row.at("end_us")), duration(row.at("service_us")) });
    }

    core::CpuServiceCost result;
    for (auto & [lane, selected] : intervals) result.add_spans(lane, std::move(selected));
    return result;
}
} // namespace markov::trace_graph::io
