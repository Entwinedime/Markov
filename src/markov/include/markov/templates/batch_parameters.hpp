#pragma once

#include "markov/contracts/control.hpp"
#include "markov/graph/source_graph.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace markov {

enum class BatchOperationRole { RelayWrite, RequestTableWrite, LastLocationConcat, PageTableDivide, LogitsReshape };
enum class PageTableMaterialization { Contiguous, SingleColumn, Strided };

// A producer-qualified native branch, selected by batch layout. Node parameter
// and dependency qualification remain separate from this semantic selector.
struct BatchOperationVariant {
    BatchOperationRole role;
    std::uint64_t source_requests;
    std::uint64_t minimum_requests;
    std::uint64_t maximum_requests;
    std::optional<PageTableMaterialization> page_table = std::nullopt;
    std::uint32_t page_size_tokens = 0;
    // The operation can use the target pool without importing source pool state.
    bool request_pool_rebindable = false;

    bool accepts(const BatchInput & input) const;
    nlohmann::json describe_parameters() const;
};

// Identity within the serving flow, independent of preceding filter calls.
enum class BatchFilterBinding { Transition, RunningUpdate };

enum class BatchRowExtent { FilterInput, FilterKept, MergeLeft, MergeRight, MergeTotal, Requests, RequestSequenceTokens };

// Operand count is independent of each tensor's row extent.
enum class BatchInputArity { Fixed, Requests };

struct BatchRowParameter {
    bool output;
    std::size_t slot;
    BatchRowExtent extent;
};

struct BatchRequestBinding {
    std::size_t request_count;
    std::size_t slot;
};

// A layout binds one row family: filter, merge, requests or one request sequence. Row slots
// are supplied by the producer; other dimensions remain observations.
class BatchParameterLayout {
    friend class TemplateAssetCodec;

public:
    BatchParameterLayout(std::optional<BatchFilterBinding> filter, std::vector<BatchRowParameter> rows,
                         std::optional<BatchRequestBinding> request = std::nullopt, BatchInputArity input_arity = BatchInputArity::Fixed)
        : filter_(filter),
          rows_(std::move(rows)),
          request_(request),
          input_arity_(input_arity) {}

    // Rebind an existing filter layout without changing its tensor or request constraints.
    BatchParameterLayout with_filter_binding(BatchFilterBinding binding) const;
    std::expected<SourceKernel, std::string> evaluate(const SourceKernel & source, const BatchInput & input,
                                                      std::optional<std::size_t> request_slot = std::nullopt) const;
    nlohmann::json describe_parameters() const;

private:
    std::optional<BatchFilterBinding> filter_;
    std::vector<BatchRowParameter> rows_;
    std::optional<BatchRequestBinding> request_;
    BatchInputArity input_arity_;
};

} // namespace markov
