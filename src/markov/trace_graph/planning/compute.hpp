#pragma once

#include "markov/trace_graph/planning/work.hpp"

#include <array>
#include <compare>
#include <string>
#include <variant>

namespace markov::trace_graph::planning {

enum class Phase { Prefill, Decode };
enum class RowBasis { QueryTokens, BatchSize };

struct RowBinding {
    RowBasis basis;
    double rows_per_item;
    auto operator<=>(const RowBinding &) const = default;
};

struct LinearGeometry {
    double n, k;
    std::array<double, 2> input_stride, weight_stride;
    auto operator<=>(const LinearGeometry &) const = default;
};

struct NormalizationGeometry {
    double width, epsilon;
    std::array<double, 2> input_stride;
    auto operator<=>(const NormalizationGeometry &) const = default;
};

struct ActivationGeometry {
    double width;
    std::array<double, 2> input_stride;
    auto operator<=>(const ActivationGeometry &) const = default;
};

struct AdditionGeometry {
    double width;
    std::array<double, 2> input_stride;
    auto operator<=>(const AdditionGeometry &) const = default;
};

struct AttentionGeometry {
    double query_heads, kv_heads, query_key_dim, value_dim;
    bool causal;
    auto operator<=>(const AttentionGeometry &) const = default;
};

using ComputeGeometry = std::variant<LinearGeometry, NormalizationGeometry, ActivationGeometry, AdditionGeometry, AttentionGeometry>;

struct RowInput {
    double rows;
};

struct AttentionInput {
    // One serial request. Grouped attention needs per-sequence lengths and is
    // outside the current adapter contract, rather than inferred from totals.
    // Context includes the query suffix; tensor_query_tokens retains padding.
    double query_tokens, tensor_query_tokens, context_tokens, batch_size, page_tokens;
};

using ComputeInput = std::variant<RowInput, AttentionInput>;

// Fixed implementation/geometry and changing execution variables stay separate.
// Shapes describe work only: no measurements, cost coefficients or native JSON.
struct ComputeOperation {
    std::string implementation;
    ComputeGeometry geometry;

    Work describe(Phase phase, const ComputeInput & input) const;
    auto operator<=>(const ComputeOperation &) const = default;
};

} // namespace markov::trace_graph::planning
