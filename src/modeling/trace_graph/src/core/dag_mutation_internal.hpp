/**
 * @file
 * @brief Shared internal primitives for prospective and applied DAG mutations.
 */
#pragma once

#include "markov/trace_graph/core/dag_mutation.hpp"

#include <functional>
#include <string_view>
#include <unordered_map>

namespace markov::trace_graph::core::dag_mutation_detail {

struct EffectEdgeKey {
    size_t src = 0;
    size_t dst = 0;
    DagEdgeKind kind = DagEdgeKind::Mutation;
    std::string effect_id;

    bool operator==(const EffectEdgeKey &) const = default;
};

struct EffectEdgeKeyHash {
    size_t operator()(const EffectEdgeKey & key) const noexcept {
        auto combine = [](size_t seed, size_t value) { return seed ^ (value + static_cast<size_t>(0x9e37'79b9'7f4a'7c15ULL) + (seed << 6U) + (seed >> 2U)); };
        size_t value = std::hash<size_t>{}(key.src);
        value = combine(value, std::hash<size_t>{}(key.dst));
        value = combine(value, std::hash<unsigned int>{}(static_cast<unsigned int>(key.kind)));
        return combine(value, std::hash<std::string>{}(key.effect_id));
    }
};

using EffectEdgeCounts = std::unordered_map<EffectEdgeKey, size_t, EffectEdgeKeyHash>;

inline EffectEdgeKey effect_edge_key(size_t src, size_t dst, DagEdgeKind kind, std::string_view effect_id) {
    return EffectEdgeKey{
        .src = src,
        .dst = dst,
        .kind = kind,
        .effect_id = std::string(effect_id),
    };
}

inline void increment_effect_edge(EffectEdgeCounts & counts, size_t src, size_t dst, DagEdgeKind kind, std::string_view effect_id) {
    if (effect_id.empty()) return;
    ++counts[effect_edge_key(src, dst, kind, effect_id)];
}

inline void decrement_effect_edge(EffectEdgeCounts & counts, size_t src, size_t dst, DagEdgeKind kind, std::string_view effect_id) {
    if (effect_id.empty()) return;
    const auto key = effect_edge_key(src, dst, kind, effect_id);
    const auto found = counts.find(key);
    if (found == counts.end()) return;
    if (found->second <= 1) counts.erase(found);
    else --found->second;
}

[[nodiscard]] std::string failure_message(const DagTopologyValidationReport & report);

} // namespace markov::trace_graph::core::dag_mutation_detail
