#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "markov/contracts/expert_routes.hpp"
#include "markov/contracts/serving.hpp"
#include "markov/templates/asset.hpp"
#include "markov/templates/instance.hpp"

namespace markov::sglang {

// Native expert ownership and token partitioning translate common routes into local call parameters.
class ExpertRouting {
public:
    ExpertRouting(std::span<const TemplateAsset * const> assets, std::span<const Request> requests, const nlohmann::json & source);
    void bind(Invocation & invocation, const Batch & batch, std::size_t rank) const;

private:
    struct Partition {
        std::int64_t rank;
        std::int64_t begin;
        std::int64_t count;
        std::int64_t experts;
    };

    struct Geometry {
        std::int64_t size;
        std::int64_t model_layers;
        std::vector<ExpertLayer> layers;
        std::map<std::size_t, std::map<std::int64_t, Partition>> ranks;
    };

    static Geometry discover(std::span<const TemplateAsset * const> assets);
    std::vector<std::int64_t> origin_counts(std::int64_t layer, std::int64_t experts, const Batch & batch, std::int64_t rank) const;

    Geometry geometry_;
    ExpertRoutes routes_;
};

} // namespace markov::sglang
