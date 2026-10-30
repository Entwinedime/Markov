#include "markov/adapters/prediction.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>

namespace markov::adapters {
namespace {

using Json = nlohmann::json;

std::size_t positive(const Json & raw, const char * name) {
    const auto value = raw.at(name).get<std::int64_t>();
    if (!raw.at(name).is_number_integer() || value <= 0) throw std::runtime_error(std::string(name) + " must be a positive integer");
    return value;
}

} // namespace

PreparedPrediction prepare_prediction(const std::filesystem::path & path) {
    std::ifstream input(path);
    const auto raw = Json::parse(input);
    const auto manifest = trace_graph::source::inspect_manifest(raw.at("manifest").get<std::string>());
    const std::filesystem::path models_path(raw.at("models").get<std::string>());
    auto selected = read_cost_models(models_path);
    if (selected.deployment != raw.at("deployment") || manifest.framework != "sglang" || selected.deployment.at("runtime") != "ascend")
        throw std::runtime_error("Prepared regional prediction needs matching SGLang/Ascend deployment and independent models");
    const auto initial = raw.at("kernel_cache_initial_state").get<std::string>();
    if (initial != "empty" && initial != "prepared") throw std::runtime_error("Kernel initial state must be empty or prepared");

    std::ifstream config(manifest.configuration_path.value());
    const auto captured = Json::parse(config);
    if (captured.at("deployment") != raw.at("deployment")) throw std::runtime_error("Base capture belongs to a different deployment");
    const auto & base = captured.at("server").at("arguments");
    const std::set<std::string> variable{ "page_size",     "max_total_tokens",     "max_prefill_tokens", "enable_hierarchical_cache",
                                          "hicache_ratio", "hicache_write_policy", "hicache_io_backend", "hicache_mem_layout" };
    std::vector<PredictionTarget> targets;
    for (const auto & target : raw.at("targets")) {
        auto configuration = base;
        for (const auto & [key, value] : target.at("configuration").items()) {
            if (!variable.contains(key) && (!base.contains(key) || base.at(key) != value))
                throw std::runtime_error("Target changes an unsupported execution setting: " + key);
            configuration[key] = value;
        }
        const auto page = positive(configuration, "page_size");
        const auto tokens = positive(configuration, "max_total_tokens");
        const auto prefill = positive(configuration, "max_prefill_tokens");
        if (tokens < page || configuration.at("hicache_io_backend") != "kernel_ascend" || configuration.at("hicache_mem_layout") != "page_first_direct")
            throw std::runtime_error("Regional cache prediction needs positive capacity and kernel_ascend/page_first_direct");
        const auto ratio = configuration.at("hicache_ratio").get<double>();
        const auto host_pages = static_cast<long double>(tokens / page) * ratio;
        if (!std::isfinite(ratio) || ratio <= 0 || host_pages < 1 || host_pages >= std::numeric_limits<std::size_t>::max())
            throw std::runtime_error("HiCache ratio must describe a finite positive host capacity");
        Json settings{
            { "device_pages",                            tokens / page },
            {   "host_pages",     static_cast<std::size_t>(host_pages) },
            { "write_policy", configuration.at("hicache_write_policy") },
            {   "io_backend",   configuration.at("hicache_io_backend") },
            {  "host_layout",   configuration.at("hicache_mem_layout") }
        };
        auto cache = sglang::read_cache_settings(settings);
        if (!configuration.at("enable_hierarchical_cache").get<bool>()) {
            cache.host_pages = 0;
            cache.backup_after_hits.reset();
        }
        const auto result_implementation =
            configuration.at("enable_hierarchical_cache").get<bool>() ? "sglang.ascend.eager:HiRadixCache" : "sglang.ascend.eager:RadixCache";
        targets.push_back({ target.at("name").get<std::string>(), std::move(configuration), page, prefill, result_implementation, std::move(cache) });
    }
    auto source = prepare_source(manifest, std::nullopt, SourceDetail::Regions);
    const auto & observations = source.context.observations();
    if (observations.counts.contains("schedule_input_facts.unsupported"))
        throw std::runtime_error("Regional prediction currently supports plain eager inputs without speculative, multimodal or logprob branches");
    const auto & workload = raw.at("workload");
    if (workload.at("requests").size() != observations.requests.size()) throw std::runtime_error("Requested workload differs from the base capture");
    for (std::size_t id = 0; id < observations.requests.size(); ++id) {
        const auto & request = workload.at("requests").at(id);
        const auto & captured_request = observations.requests[id];
        if (request.at("return_logprob").get<bool>()) throw std::runtime_error("Regional prediction does not yet model logprob output work");
        const auto prompt = observations.request_tokens.at(captured_request.identity).prompt.value().path;
        if (request.at("input_ids").is_null() || request.at("input_ids").get<std::vector<std::int64_t>>() != observations.token_paths.suffix(prompt, 0))
            throw std::runtime_error("Regional prediction currently needs input_ids matching the base's observed workload");
        std::size_t outputs = 0;
        for (const auto frame : source.context.request_frames(captured_request.identity)) {
            const auto * result = source.context.frame_result(frame);
            if (result && result->processed) outputs = std::max(outputs, result->processed->output_tokens_after.front());
        }
        if (!request.at("ignore_eos").get<bool>() || outputs != request.at("max_new_tokens").get<std::size_t>()
            || captured_request.formal != (id >= workload.at("formal_begin_index").get<std::size_t>()))
            throw std::runtime_error("Regional prediction currently requires the same fixed output count and formal workload boundary");
    }
    return { std::move(source),
             std::move(selected),
             models_path,
             initial == "empty" ? trace_graph::planning::KernelCacheInitial::Empty : trace_graph::planning::KernelCacheInitial::Prepared,
             std::move(targets) };
}

} // namespace markov::adapters
