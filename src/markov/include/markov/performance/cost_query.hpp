#pragma once

#include "markov/contracts/batch_membership.hpp"
#include "markov/contracts/service_work.hpp"

#include <nlohmann/json_fwd.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace markov {

struct AttentionCostSequence {
    std::int64_t query_tokens;
    std::int64_t prefix_tokens;

    auto operator<=>(const AttentionCostSequence &) const = default;
};

// Logical lengths and physical layout, without source/request/page identities.
// Query heads are not an operand of the cache writer and are absent for that role.
struct AttentionCostFeatures {
    AttentionOperation operation;
    std::vector<AttentionCostSequence> sequences;
    std::optional<std::int64_t> query_heads;
    std::int64_t kv_heads;
    std::int64_t head_dim;
    std::int64_t page_size_tokens;
    std::int64_t cache_pages;

    auto operator<=>(const AttentionCostFeatures &) const = default;
    static AttentionCostFeatures read(const nlohmann::json & data);
    static AttentionCostFeatures from_work(const AttentionWorkload & work);
    nlohmann::json describe() const;
};

// Ordered loop lengths and index coordinates distinguish equal tensor shapes
// with different native work. Request-pool slots are physical coordinates;
// batch selections retain their ordered positions in the input batch.
struct KernelOperandFeatures {
    std::optional<std::vector<std::int64_t>> position_lengths;
    std::optional<std::vector<std::int64_t>> logits_rows;
    std::optional<RequestPoolWork> request_pool;
    std::optional<BatchFilter> batch_filter;

    static KernelOperandFeatures from_work(const SourceKernel & work);
    static KernelOperandFeatures read(const nlohmann::json & data);
    nlohmann::json describe() const;
    bool empty() const;
    auto operator<=>(const KernelOperandFeatures &) const = default;
};

// Physical launch descriptors with the explicitly observed data-dependent work.
// A matching tensor capacity never substitutes for live sequence lengths.
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
    // Producer-declared assumption for a fixed tensor traversal. Semantic
    // values remain on target work but need not be duplicated in this cost key.
    bool tensor_traversal;
    std::optional<AttentionCostFeatures> attention = std::nullopt;
    KernelOperandFeatures operands = {};

    auto operator<=>(const KernelCostFeatures &) const = default;
    static KernelCostFeatures read(const nlohmann::json & data);
    std::array<std::string, 8> tensor_descriptor() const;
    nlohmann::json describe() const;
};

// One physical task, not a collective envelope. Rank endpoints distinguish
// routes within the recorded deployment; task IDs are only provenance.
struct CommunicationCostFeatures {
    SourceSpanKind kind;
    std::string operation;
    std::uint64_t bytes;
    std::string data_type;
    std::string transport;
    std::uint32_t source_rank;
    std::uint32_t destination_rank;

    auto operator<=>(const CommunicationCostFeatures &) const = default;
    static CommunicationCostFeatures read(const nlohmann::json & data);
    nlohmann::json describe() const;
};

enum class TransferExecution { DeviceAsync, SynchronousHostDriver };

// Device work and complete synchronous host calls have different boundaries.
// Native API identities are provenance, not cross-run matching features.
struct TransferCostFeatures {
    std::uint64_t bytes;
    TransferDirection direction;
    TransferExecution execution;

    auto operator<=>(const TransferCostFeatures &) const = default;
    static TransferCostFeatures read(const nlohmann::json & data);
    nlohmann::json describe() const;
};

// The launch variant is absent from transformed target work. Models retain the
// existing unique-observed-launch rule rather than inventing target metadata.
struct KernelCostQuery {
    std::array<std::string, 8> tensors;
    std::optional<AttentionCostFeatures> attention;
    KernelOperandFeatures operands;
    std::vector<std::string> unmodeled_operands;
    const SourceKernel * work; // Borrowed for this query and its immediate report.

    static KernelCostQuery from_work(const SourceKernel & work);
    nlohmann::json describe() const;
};

// An unknown physical service remains explicit instead of receiving a default.
struct UnresolvedCostQuery {
    SourceSpanKind node_kind;

    static UnresolvedCostQuery from_work(const ServiceWork & work);
    nlohmann::json describe() const;
};

enum class CostQueryKind { Undefined, Kernel, Communication, Transfer, DeviceCommand };

enum class CostQueryIssue { None, FeaturesUndefined };

using CostQueryFeatures = std::variant<KernelCostQuery, CommunicationCostFeatures, TransferCostFeatures, DeviceCommandWork, UnresolvedCostQuery>;

struct CostObject {
    CostQueryKind kind;
    CostQueryFeatures features;
    CostQueryIssue issue = CostQueryIssue::None;
    std::string_view contract_gap = {};

    static std::string_view kind_name(CostQueryKind kind);
    nlohmann::json describe() const;
};

// Feature extraction is shared by coverage reporting and every model lookup.
// Host control services expose no performance object in the current scope.
struct CostQuery {
    std::vector<CostObject> objects;

    static CostQuery from_work(const ServiceWork & work);
};

} // namespace markov
