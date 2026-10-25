#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "markov/contracts/serving.hpp"

namespace markov {

struct ExpertLayer {
    std::int64_t id;
    std::int64_t experts;
    std::int64_t top_k;
};

// Conditional routes use logical token positions, independent of batches and physical workers.
class ExpertRoutes {
public:
    ExpertRoutes(const nlohmann::json & source, std::span<const Request> requests, std::span<const ExpertLayer> layers, std::int64_t model_layers,
                 std::int64_t padding_tokens);

    std::vector<std::int64_t> counts(std::size_t request, std::int64_t layer, std::int64_t begin, std::int64_t end) const;
    std::vector<std::int64_t> padding_counts(std::int64_t layer, std::int64_t begin, std::int64_t end) const;

private:
    struct Segment {
        std::int64_t begin;
        std::int64_t end;
        std::vector<std::int64_t> experts;
    };
    using LayerSegments = std::map<std::int64_t, std::vector<Segment>>;

    void insert(const nlohmann::json & source, LayerSegments & target, std::int64_t tokens, std::int64_t model_layers) const;
    void require_coverage(LayerSegments & target, std::int64_t tokens, const std::string & context) const;
    std::vector<std::int64_t> counts(const LayerSegments & routes, std::int64_t layer, std::int64_t begin, std::int64_t end) const;

    std::map<std::int64_t, ExpertLayer> layers_;
    std::vector<LayerSegments> requests_;
    LayerSegments padding_;
};

} // namespace markov
