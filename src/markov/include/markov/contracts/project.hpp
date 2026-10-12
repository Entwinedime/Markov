#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace markov {

enum class Framework { SGLang, KTransformers };
enum class Scope { Modeled, HttpE2E };
enum class CacheState { Cold, Warm };

struct Request {
    std::string id;
    std::int64_t arrival_time_ns;
    std::variant<std::vector<std::uint32_t>, std::string> input;
    std::uint32_t output_tokens;
};

struct SGLangConfiguration {
    std::string device;
    std::uint32_t tp_size;
    std::string attention_backend;

    std::uint32_t max_running_requests;
    std::uint32_t max_total_tokens;
    std::int32_t chunked_prefill_size;
    std::uint32_t max_prefill_tokens;
    std::string schedule_policy;

    bool disable_radix_cache;
    bool disable_overlap_schedule;
    bool disable_cuda_graph;
    bool enable_mixed_chunk;
    bool enable_hierarchical_cache;

    static SGLangConfiguration from_parameters(const nlohmann::json & parameters);
    nlohmann::json parameters() const;

    bool operator==(const SGLangConfiguration &) const = default;
};

struct KTransformersConfiguration {
    std::string backend_type;
    std::uint32_t tp;
    std::uint32_t max_batch_size;
    std::uint32_t chunk_size;
    std::uint32_t cpu_infer;
    std::uint32_t cache_lens;
    std::string sched_strategy;
    bool use_cuda_graph;

    std::filesystem::path gguf_path;
    std::filesystem::path optimize_config_path;
};

using FrameworkConfiguration = std::variant<SGLangConfiguration, KTransformersConfiguration>;

struct CollectionPermission {
    bool allow_run = false;
    std::int64_t max_wall_time_sec = 0;
};

struct Environment {
    std::string service;
    std::vector<std::uint32_t> devices;
};

struct Candidate {
    std::string id;
    Framework framework;
    Environment environment;
    std::filesystem::path config_path;
    FrameworkConfiguration configuration;
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

    std::string controller = "lightweight";
    Scope scope = Scope::Modeled;
    CacheState initial_cache = CacheState::Cold;

    CollectionPermission base_collection;
    CollectionPermission performance_collection;
    std::vector<std::filesystem::path> performance_manifests;

    std::filesystem::path output_directory;
    bool diagnostics = false;

    const Candidate & configuration(std::string_view id) const;
    const RequestWorkload & workload(std::string_view id) const;
};

class ProjectReader {
public:
    Project read(const std::filesystem::path & path) const;
};

} // namespace markov
