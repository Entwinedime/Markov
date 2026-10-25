#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
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

struct RankMessage {
    TargetRegion region;
    Invocation invocation;
};

// Native slot/page encoding stays here; the controller owns the logical allocation.
class RankStructure {
public:
    RankStructure(const TemplateAsset & asset, ServingSettings settings, RegionGraph & graph, std::size_t execution_offset, std::size_t rank,
                  std::size_t lanes);
    RankStructure(const RankStructure &) = delete;
    RankStructure & operator=(const RankStructure &) = delete;

    std::size_t admission_members(std::size_t lane) const;
    RankProgram expand(const Batch & batch, const BatchEffects & effects);
    std::map<std::string, std::size_t> prepare(const Batch & batch, std::size_t lane);
    TargetRegion model(const Batch & batch, const std::map<std::string, std::size_t> & inputs);
    RankProgram complete(const Batch & batch, const BatchEffects & effects, std::size_t lane, const std::map<std::string, std::size_t> & model_columns,
                         std::size_t sampled_tokens);
    RankMessage message(const std::string & role, const Batch & batch, const std::map<std::string, std::size_t> & payload, bool sending);
    void wait_message(const std::string & channel, const Batch & batch, const RankMessage & message);
    std::size_t receive_result(const Batch & batch, std::size_t sampled_tokens);
    std::optional<std::size_t> idle();

    void live_state(std::vector<std::size_t *> & values, std::set<std::size_t> & storage);

private:
    Invocation invocation(const Batch & batch) const;
    void stamp(std::size_t region_begin);
    void return_pages(std::size_t output, std::span<const std::size_t> pages);

    ServingSettings settings_;
    const TemplateAsset & asset_;
    RegionGraph & shared_;
    TargetGraph graph_;
    std::vector<BatchViews> views_;
    std::size_t rank_;
    bool publishes_;

    std::optional<std::size_t> free_pages_;
    std::optional<std::size_t> request_map_;
    std::optional<std::size_t> token_relay_;
};

} // namespace markov::sglang
