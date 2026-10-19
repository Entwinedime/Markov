#pragma once

#include "markov/templates/tensor_parameters.hpp"

#include <compare>
#include <utility>

namespace markov {

class AttentionParameterRule {
    friend class TemplateAssetCodec;

public:
    AttentionParameterRule(TensorParameterLayout shapes, AttentionWorkload workload) : shapes_(std::move(shapes)), workload_(std::move(workload)) {}
    SourceKernel evaluate(const SourceKernel & source, const BatchInput & input) const;
    auto operator<=>(const AttentionParameterRule &) const = default;
    const AttentionWorkload & geometry() const { return workload_; }

private:
    TensorParameterLayout shapes_;
    AttentionWorkload workload_; // Fixed operation and cache geometry; target sequences are supplied per instance.
};


} // namespace markov
