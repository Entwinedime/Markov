#pragma once

#include "markov/adapters/configuration.hpp"
#include "markov/contracts/request.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace markov {

struct CollectionPermission {
    bool allow_run = false;
    std::int64_t max_wall_time_sec = 0;
};

struct RequestWorkload {
    std::string id;
    std::filesystem::path path;
    std::vector<Request> requests;
};

struct TemplateInput {
    std::filesystem::path manifest;
    std::optional<std::filesystem::path> node_durations;
};

// Paths are resolved relative to the file declaring them. Project owns all inputs.
struct Project {
    std::filesystem::path path;
    std::filesystem::path model_path;
    std::filesystem::path tokenizer_path;
    std::vector<Candidate> configurations;
    std::vector<RequestWorkload> request_workloads;
    std::vector<TemplateInput> templates;

    CollectionPermission base_collection;
    CollectionPermission performance_collection;
    std::vector<std::filesystem::path> performance_manifests;
    std::vector<std::filesystem::path> cost_models;

    std::filesystem::path output_directory;
    bool diagnostics = false;

    const Candidate & configuration(std::string_view id) const;
    const RequestWorkload & workload(std::string_view id) const;
};

} // namespace markov
