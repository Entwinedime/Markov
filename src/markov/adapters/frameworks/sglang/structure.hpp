#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "markov/adapters/frameworks/sglang/expert_routing.hpp"
#include "markov/adapters/frameworks/sglang/rank_structure.hpp"
#include "markov/adapters/frameworks/sglang/topology.hpp"
#include "markov/templates/batch_expansion.hpp"
#include "markov/templates/parallel_composition.hpp"

namespace markov::sglang {

// Controller actions select rank groups; each rank retains only its local graph ports.
class ServingStructure final : public BatchExpansion {
public:
    ServingStructure(RegionGraph & graph, std::span<const TemplateAsset> assets, ServingSettings settings, std::span<const Request> requests,
                     const ServingTopology & topology, const std::string & instance, const nlohmann::json & routes);
    ServingStructure(const ServingStructure &) = delete;
    ServingStructure & operator=(const ServingStructure &) = delete;

    GroupAdmission admission() const override;
    BatchProgram expand(const ServingAction & action) override;
    BatchProgram expand(const ServingAction & action, const std::map<std::size_t, std::vector<std::size_t>> & prerequisites);
    std::vector<std::size_t> idle() override;
    const RegionGraph & graph() const override;
    std::vector<std::size_t> retire() override;
    nlohmann::json summary() const;
    nlohmann::json document() const;
    PageReuse page_reuse() const;
    std::span<const TemplateAsset * const> sources() const;
    TargetRegion transfer(std::size_t rank, std::int64_t bytes, std::int64_t segments);
    void live_state(std::vector<std::size_t *> & values, std::set<std::size_t> & storage);

private:
    struct FlowViews {
        std::map<std::size_t, std::map<std::string, std::size_t>> inputs;
        std::map<std::size_t, std::map<std::string, std::size_t>> outputs;
    };

    BatchProgram pipeline_stage(const ServingAction & action, std::vector<RankWork> & work);
    BatchProgram pipeline_result(const ServingAction & action, std::vector<RankWork> & work);

    RegionGraph & graph_;

    // Assembly owns the common target topology; the adapter borrows its immutable bindings.
    const ServingTopology & topology_;
    const RankGroup & instance_;
    ParallelComposition parallel_;
    std::int64_t prefill_token_granularity_;
    std::vector<const TemplateAsset *> assets_;

    // Rank builders borrow this immutable routing condition for the lifetime of the expansion.
    std::optional<ExpertRouting> routing_;

    // Views borrow their rank's builder, so rank objects retain stable addresses.
    std::map<std::size_t, std::unique_ptr<RankStructure>> ranks_;
    std::map<std::size_t, FlowViews> flows_;
    std::size_t batch_count_ = 0;
    std::size_t action_count_ = 0;
    std::size_t control_count_ = 0;
    std::size_t operation_count_ = 0;
    std::size_t node_count_ = 0;
    std::size_t dependency_count_ = 0;
};

} // namespace markov::sglang
