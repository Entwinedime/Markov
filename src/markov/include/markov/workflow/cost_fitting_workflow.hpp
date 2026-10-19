#pragma once

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <string_view>

namespace markov {

class CostFittingWorkflow {
public:
    nlohmann::json run(const std::filesystem::path & manifest_path, std::string_view unit = "all") const;
    nlohmann::json evaluate(const std::filesystem::path & candidate_path, const std::filesystem::path & manifest_path) const;

private:
    struct Observation;
    struct Measurements;
    Observation read(const std::filesystem::path & manifest_path) const;
    Measurements select(const Observation & source, std::string_view unit) const;
    void read_host_transfers(Measurements & data, const Observation & source) const;
    void read_cpu(Measurements & data, const Observation & source) const;
    void read_device(Measurements & data, const Observation & source, std::string_view unit) const;
    nlohmann::json fit(const Observation & source, std::string_view unit) const;
};

} // namespace markov
