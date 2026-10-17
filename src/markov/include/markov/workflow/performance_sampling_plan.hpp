#pragma once

#include <nlohmann/json_fwd.hpp>

#include <span>
#include <vector>

namespace markov {

class CollectionAdapter;
class TemplateAdapter;
struct Project;

// Merge actual named requirements and hand bounded native work to the existing
// framework recipe. Source prototypes and alternative billing cuts do not by
// themselves authorize another measurement.
class PerformanceSamplingPlan {
public:
    PerformanceSamplingPlan(std::span<const CollectionAdapter * const> collection_adapters, std::span<const TemplateAdapter * const> template_adapters);

    nlohmann::json run(const Project & project, const nlohmann::json & prediction) const;

private:
    std::vector<const CollectionAdapter *> collection_adapters_;
    std::vector<const TemplateAdapter *> template_adapters_;
};

} // namespace markov
