#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "markov/contracts/serving.hpp"
#include "markov/templates/target_graph.hpp"

namespace markov::sglang {

// Views own graph references and supplied row selections, never request or KV state.
class BatchViews {
public:
    BatchViews(TargetGraph & graph, const TemplateAsset & asset);

    std::size_t admission_members() const;
    void advance(Phase phase);
    void join_prefill(Phase phase);
    std::size_t select(std::span<const BatchMember> members);
    std::optional<std::size_t> idle();
    std::map<std::string, std::size_t> decode_inputs() const;
    void remember(Phase phase, const std::map<std::string, std::size_t> & ports, const BatchEffects & effects);
    void live_values(std::vector<std::size_t *> & values);

private:
    struct View {
        std::map<std::string, std::size_t> columns;
        std::vector<std::size_t> requests;
        std::vector<std::int64_t> kept_rows;
    };

    std::map<std::string, std::size_t> columns(const std::map<std::string, std::size_t> & ports) const;
    std::size_t filter(View & view, const std::string & phase);
    void merge(View & left, const View & right, const std::string & phase);

    TargetGraph & graph_;
    std::set<std::string> fields_;
    std::optional<View> running_;
    std::optional<View> prefill_;
};

} // namespace markov::sglang
