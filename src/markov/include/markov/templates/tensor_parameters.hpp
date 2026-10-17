#pragma once

#include "markov/contracts/control.hpp"
#include "markov/graph/source_graph.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace markov {

enum class TensorAxisExtent { Tokens, Requests, SequenceTokens, PageColumns, SequenceStorage, RequestPoolRows };

struct TensorParameterAxis {
    std::size_t slot;
    std::int64_t multiplier;
    TensorAxisExtent extent = TensorAxisExtent::Tokens;
    std::size_t axis = 0;
    auto operator<=>(const TensorParameterAxis &) const = default;
};

// Qualified operand axes only; constant shapes, formats and identity are retained.
class TensorParameterLayout {
    friend class TemplateAssetCodec;

public:
    TensorParameterLayout(std::vector<TensorParameterAxis> inputs, std::vector<TensorParameterAxis> outputs, std::int64_t page_size_tokens)
        : inputs_(std::move(inputs)),
          outputs_(std::move(outputs)),
          page_size_tokens_(page_size_tokens) {}
    SourceKernel evaluate(const SourceKernel & source, const BatchInput & input, std::optional<std::uint32_t> request_capacity = std::nullopt) const;
    nlohmann::json describe_parameters() const;
    std::int64_t page_size_tokens() const { return page_size_tokens_; }
    auto operator<=>(const TensorParameterLayout &) const = default;

private:
    std::vector<TensorParameterAxis> inputs_;
    std::vector<TensorParameterAxis> outputs_;
    std::int64_t page_size_tokens_;
};

} // namespace markov
