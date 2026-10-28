#pragma once

#include "markov/trace_graph/planning/work.hpp"

#include <array>
#include <compare>
#include <optional>
#include <string>
#include <variant>
#include <vector>

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

struct CacheWriteGeometry {
    std::string operation;
    double width, source_row_stride;
    std::string materialize_implementation, materialized_write_implementation;
    auto operator<=>(const CacheWriteGeometry &) const = default;
};

// These factors count tensor elements, not time. The adapter supplies native
// geometry; the same description binds query length and batch size independently.
struct TensorGeometry {
    std::string operation;
    Features fixed, per_query_token, per_request;
    auto operator<=>(const TensorGeometry &) const = default;
};

struct ReshapeGeometry {
    double width, head_dim, input_row_stride;
    bool is_view(double rows) const;
    auto operator<=>(const ReshapeGeometry &) const = default;
};

using ComputeGeometry = std::variant<LinearGeometry, NormalizationGeometry, ActivationGeometry, AdditionGeometry, AttentionGeometry, CacheWriteGeometry,
                                     TensorGeometry, ReshapeGeometry>;

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

    bool uses_frame_input() const;
    std::vector<Work> describe(Phase phase, const ComputeInput & input) const;
    auto operator<=>(const ComputeOperation &) const = default;
};

struct ComputePart {
    ComputeOperation operation;
    std::optional<RowBinding> rows;
    auto operator<=>(const ComputePart &) const = default;
};

} // namespace markov::trace_graph::planning
