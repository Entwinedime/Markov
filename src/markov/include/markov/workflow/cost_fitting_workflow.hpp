#pragma once

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <string_view>

namespace markov {

class CostFittingWorkflow {
public:
    nlohmann::json run(const std::filesystem::path & manifest_path, std::string_view unit = "admission") const;
    nlohmann::json evaluate(const std::filesystem::path & candidate_path, const std::filesystem::path & manifest_path) const;

private:
    struct Measurements;
    Measurements read(const std::filesystem::path & manifest_path, std::string_view unit) const;
    void read_admission(Measurements & data, const std::filesystem::path & manifest_path) const;
    void read_workers(Measurements & data, const std::filesystem::path & manifest_path) const;
    void read_device(Measurements & data, const std::filesystem::path & manifest_path, std::string_view unit) const;
};

} // namespace markov
