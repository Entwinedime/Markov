#pragma once

#include "markov/contracts/batch_membership.hpp"
#include "markov/graph/source_graph.hpp"
#include "markov/templates/batch_parameters.hpp"

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>

namespace markov {

struct NativeFilterSelection {
    std::size_t index;
    BatchFilterBinding role;
};

class BatchLayout {
public:
    std::expected<std::optional<BatchParameterLayout>, std::string> bind(const SourceKernel & source, std::span<const BatchFilter> filters,
                                                                         const std::optional<BatchMerge> & merge,
                                                                         std::optional<NativeFilterSelection> selection) const;
    std::expected<std::optional<BatchParameterLayout>, std::string> bind_decode_lengths(const SourceKernel & source, std::size_t requests) const;
    std::expected<std::optional<BatchParameterLayout>, std::string> bind_merge(const SourceKernel & source, const std::optional<BatchMerge> & observed) const;

private:
    std::expected<std::optional<BatchParameterLayout>, std::string> filter(const SourceKernel & source, std::span<const BatchFilter> filters,
                                                                           std::optional<NativeFilterSelection> selection) const;
};

} // namespace markov
