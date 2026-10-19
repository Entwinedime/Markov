#pragma once

#include "markov/contracts/control.hpp"
#include "markov/graph/source_graph.hpp"
#include "markov/templates/tensor_parameters.hpp"

#include <compare>
#include <cstdint>
#include <utility>
#include <vector>

namespace markov {

enum class ModelWorkload { None, SequenceMaximum, SequenceLengths, PageTable, Positions, LastPositions, LogitsIndices };

// Immutable target parameter rule. Native provenance is kept separately.
class ModelParameterRule {
    friend class TemplateAssetCodec;

public:
    ModelParameterRule(std::vector<TensorParameterAxis> inputs, std::vector<TensorParameterAxis> outputs, std::int64_t page_size, ModelWorkload workload)
        : shapes_(std::move(inputs), std::move(outputs), page_size),
          workload_(workload) {}
    SourceKernel evaluate(const SourceKernel & source, const BatchInput & input) const;
    auto operator<=>(const ModelParameterRule &) const = default;

private:
    TensorParameterLayout shapes_;
    ModelWorkload workload_;
};


} // namespace markov
