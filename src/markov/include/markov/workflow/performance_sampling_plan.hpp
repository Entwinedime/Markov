#pragma once

#include <nlohmann/json_fwd.hpp>

#include <span>

namespace markov {

class CollectionAdapter;
class TemplateAdapter;
struct Project;

// Merge actual named requirements and hand bounded native work to the existing
// framework recipe. Source prototypes and alternative billing cuts do not by
// themselves authorize another measurement.
class PerformanceSamplingPlan {
public:
    PerformanceSamplingPlan(std::span<const CollectionAdapter * const> collection_adapters, std::span<const TemplateAdapter * const> template_adapters)
        : collection_adapters_(collection_adapters),
          template_adapters_(template_adapters) {}

    nlohmann::json run(const Project & project, const nlohmann::json & prediction, const nlohmann::json & attempts) const;

private:
    // The entry point owns adapters throughout preparation.
    std::span<const CollectionAdapter * const> collection_adapters_;
    std::span<const TemplateAdapter * const> template_adapters_;
};

} // namespace markov
