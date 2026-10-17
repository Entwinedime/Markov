#pragma once

#include "markov/contracts/control.hpp"
#include "markov/graph/source_graph.hpp"
#include "markov/templates/tensor_parameters.hpp"

#include <nlohmann/json.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace markov {

enum class ModelKernelRole {
    SequenceLengthMaximum,
    SequenceLengthsCast,
    PageTableView,
    PageTableSlice,
    PageTableCast,
    PageTableDivide,
    ForwardPositions,
    ForwardInputCopies,
    DecodePositionOffset,
    DecodePositionClamp,
    QkvProjection,
    QkvSlice,
    Rope,
    PositionGather,
    PositionTile,
    PositionSlice,
    CacheSlotCast,
    ValueSlice,
    EmbeddingPointwise,
    EmbeddingGather,
    EmbeddingMask,
    LogitsLengthsCast,
    LogitsPrefixSum,
    LogitsLastIndex,
    LogitsSelect,
    LogitsProjection,
    LogitsCast,
    LogitsTranspose,
    SamplingPositions,
    GreedySelection,
    GreedyTokenCast,
    AttentionOutput,
    MlpGateUp,
    MlpActivation,
    MlpDown,
    HiddenNorm,
    ResidualNorm,
    ModelOutputNorm,
    QueryNorm,
    KeyNorm,
    FusedQkv
};

enum class ModelWorkload { None, SequenceMaximum, SequenceLengths, PageTable, Positions, LastPositions, LogitsIndices };

// Immutable target parameter rule. Native provenance is kept separately.
class ModelParameterRule {
    friend class TemplateAssetCodec;

public:
    ModelParameterRule(ModelKernelRole role, std::vector<TensorParameterAxis> inputs, std::vector<TensorParameterAxis> outputs, std::int64_t page_size,
                       ModelWorkload workload, bool partial_operands)
        : role_(role),
          shapes_(std::move(inputs), std::move(outputs), page_size),
          workload_(workload),
          partial_operands_(partial_operands) {}
    SourceKernel evaluate(const SourceKernel & source, const BatchInput & input) const;
    nlohmann::json describe_parameters() const;
    ModelKernelRole role() const { return role_; }
    bool has_partial_operands() const { return partial_operands_; }
    auto operator<=>(const ModelParameterRule &) const = default;

private:
    ModelKernelRole role_;
    TensorParameterLayout shapes_;
    ModelWorkload workload_;
    bool partial_operands_;
};

// The node slot selects a rule through the source graph, while annotations keep
// the original native evidence without participating in rule equivalence.
struct ModelParameterObservation {
    std::size_t node; // Source node slot, unchanged by region sharing.
    nlohmann::json annotations;
};

} // namespace markov
