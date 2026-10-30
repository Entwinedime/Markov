#pragma once

#include "markov/adapters/probes.hpp"

#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace markov::adapters::ktransformers {

class ProbeReader final : public adapters::ProbeReader {
public:
    ProbeReader(trace_graph::source::Observations & output, const std::optional<std::filesystem::path> & configuration);

private:
    void observe(const ProbeEvent & event) override;
    std::optional<nlohmann::json> configuration_;
    std::map<std::string, std::size_t> iterations_;
    std::map<std::string, trace_graph::source::Time> phase_durations_;
};

} // namespace markov::adapters::ktransformers
