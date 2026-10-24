#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <nlohmann/json.hpp>

#include "markov/adapters/frameworks/sglang/rank_structure.hpp"
#include "markov/templates/batch_expansion.hpp"
#include "markov/templates/parallel_composition.hpp"

namespace markov::sglang {

// One controller decision drives all native roles; ranks only retain their own graph ports.
class ServingStructure final : public BatchExpansion {
public:
    ServingStructure(std::span<const TemplateAsset> assets, ServingSettings settings, std::span<const Request> requests);
    ServingStructure(const ServingStructure &) = delete;
    ServingStructure & operator=(const ServingStructure &) = delete;

    AdmissionConstraints admission() const override;
    BatchProgram expand(const Batch & batch, const BatchEffects & effects) override;
    std::vector<std::size_t> idle() override;
    const RegionGraph & graph() const override;
    std::vector<std::size_t> retire() override;
    nlohmann::json summary() const;
    nlohmann::json document() const;

private:
    RegionGraph graph_;
    ParallelComposition parallel_;
    std::int64_t prefill_token_granularity_;
    std::vector<const TemplateAsset *> assets_;

    // Views borrow their rank's builder, so rank objects retain stable addresses.
    std::vector<std::unique_ptr<RankStructure>> ranks_;
    std::size_t batch_count_ = 0;
    std::size_t control_count_ = 0;
    std::size_t operation_count_ = 0;
    std::size_t node_count_ = 0;
    std::size_t dependency_count_ = 0;
};

} // namespace markov::sglang
