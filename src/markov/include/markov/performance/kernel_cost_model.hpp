#pragma once

#include "markov/performance/cost_model.hpp"
#include "markov/performance/cost_query.hpp"

#include <nlohmann/json_fwd.hpp>

#include <array>
#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace markov {

// An exact tensor and operand candidate. A target without launch metadata uses
// the sole observed launch variant conditionally, never an average of variants.
class KernelCostModel final : public CostModel {
public:
    explicit KernelCostModel(const nlohmann::json & entries);

    std::optional<std::int64_t> measured_cost(const KernelCostFeatures & features) const;
    CostModelResult estimate(const CostQuery & query) const override;
    NodeCostBasis basis() const override { return NodeCostBasis::KernelCandidate; }

private:
    struct Descriptor {
        std::array<std::string, 8> tensors;
        std::optional<AttentionCostFeatures> attention;
        KernelOperandFeatures operands;

        auto operator<=>(const Descriptor &) const = default;
    };
    struct Launch {
        std::string task_type;
        std::int64_t blocks;
        std::int64_t duration_ns;
        bool tensor_traversal;
    };
    std::map<Descriptor, std::vector<Launch>> entries_;
};

} // namespace markov
