#pragma once

#include <cstddef>
#include <optional>
#include <set>
#include <span>
#include <vector>

#include "markov/adapters/frameworks/sglang/batches.hpp"
#include "markov/contracts/serving.hpp"
#include "markov/graph/region_graph.hpp"
#include "markov/templates/target_graph.hpp"

namespace markov::sglang {

struct RankProgram {
    std::optional<std::size_t> publication;
    std::size_t completion;
    std::vector<std::size_t> returned_pages;
};

// Native slot/page encoding stays here; the controller owns the logical allocation.
class RankStructure {
public:
    RankStructure(const TemplateAsset & asset, ServingSettings settings, RegionGraph & graph, std::size_t execution_offset, std::size_t rank);
    RankStructure(const RankStructure &) = delete;
    RankStructure & operator=(const RankStructure &) = delete;

    std::size_t admission_members() const;
    RankProgram expand(const Batch & batch, const BatchEffects & effects);
    std::optional<std::size_t> idle();

    void live_state(std::vector<std::size_t *> & values, std::set<std::size_t> & storage);

private:
    std::size_t prepare(const Batch & batch, const BatchEffects & effects);
    RankProgram complete(const Batch & batch, const BatchEffects & effects, std::size_t sampled_tokens);
    void return_pages(std::size_t output, std::span<const std::size_t> pages);

    ServingSettings settings_;
    RegionGraph & shared_;
    TargetGraph graph_;
    BatchViews views_;
    std::size_t rank_;

    std::optional<std::size_t> free_pages_;
    std::optional<std::size_t> request_map_;
    std::optional<std::size_t> token_relay_;
};

} // namespace markov::sglang
