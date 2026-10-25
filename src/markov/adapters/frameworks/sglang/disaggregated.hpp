#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <vector>

#include "markov/adapters/frameworks/sglang/rank_structure.hpp"
#include "markov/controller/disaggregated.hpp"
#include "markov/templates/batch_expansion.hpp"
#include "markov/templates/parallel_composition.hpp"

namespace markov::sglang {

// Each instance translates its native calls; only explicit data-ready ports cross the PD boundary.
class DisaggregatedStructure final : public BatchExpansion {
public:
    DisaggregatedStructure(std::span<const TemplateAsset> assets, ServingSettings prefill, ServingSettings decode, std::span<const Request> requests,
                           const DisaggregatedGroups & groups, std::span<const std::int64_t> devices);

    GroupAdmission admission() const override;
    BatchProgram expand(const ServingAction & action) override;
    std::vector<std::size_t> idle() override;
    const RegionGraph & graph() const override;
    std::vector<std::size_t> retire() override;
    nlohmann::json summary() const;
    nlohmann::json document() const;
    PageReuse page_reuse() const;

private:
    struct Handoff {
        std::vector<std::size_t> reserved;
        std::vector<std::size_t> prepared;
        std::vector<std::size_t> transferred;
        bool accepted = false;
        bool released = false;
    };

    void require(std::span<const std::size_t> producers, std::size_t begin, const std::string & kind);
    std::vector<std::size_t> transfer(const ServingAction & action, const Handoff & handoff);
    void count(std::size_t operation_begin, std::size_t node_begin, std::size_t dependency_begin);

    RegionGraph graph_;
    ParallelTopology topology_;
    ParallelComposition parallel_;
    std::vector<const TemplateAsset *> assets_;
    std::vector<std::unique_ptr<RankStructure>> ranks_;
    std::map<std::size_t, Handoff> handoffs_;
    std::size_t batches_ = 0;
    std::size_t actions_ = 0;
    std::size_t operations_ = 0;
    std::size_t nodes_ = 0;
    std::size_t dependencies_ = 0;
    std::int64_t kv_bytes_ = 0;
    std::int64_t metadata_bytes_ = 0;
};

} // namespace markov::sglang
