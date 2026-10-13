#pragma once

#include "markov/performance/cost_model.hpp"

#include <nlohmann/json_fwd.hpp>

#include <array>
#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace markov {

// Physical launch descriptors only. Data-dependent work (for example live KV
// lengths) is not inferred from tensor capacity or a matching kernel name.
struct KernelCostFeatures {
    std::string name;
    std::string operation;
    std::string input_shapes;
    std::string output_shapes;
    std::string input_dtypes;
    std::string output_dtypes;
    std::string input_formats;
    std::string output_formats;
    std::string task_type;
    std::int64_t launch_blocks;

    auto operator<=>(const KernelCostFeatures &) const = default;
    static KernelCostFeatures read(const nlohmann::json & data);
    nlohmann::json describe() const;
};

// An exact tensor-descriptor candidate. A target without launch metadata uses
// the sole observed launch variant conditionally, never an average of variants.
class KernelCostModel final : public CostModel {
public:
    explicit KernelCostModel(const nlohmann::json & entries);

    std::optional<std::int64_t> measured_cost(const KernelCostFeatures & features) const;
    std::optional<std::int64_t> estimate(const ServiceWork & work) const override;
    NodeCostBasis basis() const override { return NodeCostBasis::KernelCandidate; }

private:
    using Descriptor = std::array<std::string, 8>;
    struct Launch {
        std::string task_type;
        std::int64_t blocks;
        std::int64_t duration_ns;
    };
    std::map<Descriptor, std::vector<Launch>> entries_;
};

} // namespace markov
