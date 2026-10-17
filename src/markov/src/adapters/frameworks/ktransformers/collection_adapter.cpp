#include "markov/adapters/frameworks/ktransformers/collection_adapter.hpp"

#include "markov/contracts/json_input.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <stdexcept>
#include <utility>
#include <variant>

namespace markov {

FrameworkConfiguration KTransformersCollectionAdapter::read_configuration(const std::filesystem::path & path, const nlohmann::json & data) const {
    using input::fields;
    using input::positive;
    using input::resolve_path;
    using input::string_value;

    input::fields(data, { "parameters", "model_assets" });
    const auto & p = data.at("parameters");
    const auto assets = data.value("model_assets", nlohmann::json::object());

    fields(p, { "backend_type", "tp", "max_batch_size", "chunk_size", "cpu_infer", "cache_lens", "sched_strategy", "use_cuda_graph" });
    fields(assets, { "gguf_path", "optimize_config_path" });

    return KTransformersConfiguration{ string_value(p.at("backend_type")),
                                       positive(p.at("tp")),
                                       positive(p.at("max_batch_size")),
                                       positive(p.at("chunk_size")),
                                       positive(p.at("cpu_infer")),
                                       positive(p.at("cache_lens")),
                                       string_value(p.at("sched_strategy")),
                                       p.at("use_cuda_graph").get<bool>(),
                                       resolve_path(path, assets.at("gguf_path")),
                                       resolve_path(path, assets.at("optimize_config_path")) };
}

nlohmann::json KTransformersCollectionAdapter::configuration(const FrameworkConfiguration & configuration) const {
    const auto & value = std::get<KTransformersConfiguration>(configuration);
    return {
        {   "parameters",
         { { "backend_type", value.backend_type },
         { "tp", value.tp },
         { "max_batch_size", value.max_batch_size },
         { "chunk_size", value.chunk_size },
         { "cpu_infer", value.cpu_infer },
         { "cache_lens", value.cache_lens },
         { "sched_strategy", value.sched_strategy },
         { "use_cuda_graph", value.use_cuda_graph } }                                                                                   },
        { "model_assets", { { "gguf_path", value.gguf_path.string() }, { "optimize_config_path", value.optimize_config_path.string() } } }
    };
}

nlohmann::json KTransformersCollectionAdapter::instrumentation() const {
    // Native KT wrappers already run in Python; no standard template binding is
    // claimed by this capture-only adapter.
    return {
        { "block_bindings", nlohmann::json::array() },
        { "native_regions",                 nullptr },
        {         "blocks", nlohmann::json::array() }
    };
}

nlohmann::json KTransformersCollectionAdapter::workloads(const Candidate &, std::span<const Request>, const nlohmann::json &) const {
    throw std::invalid_argument("independent sampling currently implements SGLang");
}

nlohmann::json KTransformersCollectionAdapter::sampling_requests(const Candidate &, std::span<const Request>, const nlohmann::json & requirements) const {
    auto unresolved = nlohmann::json::array();
    for (std::size_t index = 0; index < requirements.size(); ++index)
        unresolved.push_back({
            { "requirement_index",                                            index },
            {            "reason", "framework_performance_sampling_not_implemented" }
        });
    return {
        { "measurement_requests", nlohmann::json::array() },
        {          "assignments", nlohmann::json::array() },
        {           "unresolved",   std::move(unresolved) }
    };
}

nlohmann::json KTransformersCollectionAdapter::collectives(const Candidate &, std::span<const std::filesystem::path>) const {
    throw std::invalid_argument("communication sampling currently implements SGLang");
}

} // namespace markov
