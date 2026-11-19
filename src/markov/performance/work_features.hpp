#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "markov/contracts/expression.hpp"
#include "markov/graph/graph.hpp"

namespace markov {

class WorkFeatures {
public:
    explicit WorkFeatures(const GraphScope & scope);
    explicit WorkFeatures(Parameters parameters);

    std::string_view phase() const { return phase_; }
    std::optional<double> value(std::string_view axis) const;
    nlohmann::json document() const;

private:
    enum BatchAxis { Members, QueryTokens, ContextTokens, MaxContextTokens, MaxQueryTokens, AttentionPairs, BatchAxisCount };
    static constexpr std::array<std::string_view, BatchAxisCount> batch_names_{
        "batch.members", "batch.query_tokens", "batch.context_tokens", "batch.max_context_tokens",
        "batch.max_query_tokens", "batch.attention_pairs",
    };
    static constexpr std::string_view parameter_prefix_ = "parameter.";

    std::span<const std::string_view> batch_axes() const { return std::span(batch_names_).first(batch_axis_count_); }

    std::map<std::string, std::int64_t, std::less<>> parameters_;
    std::string_view phase_;
    std::array<double, BatchAxisCount> batch_{};
    std::size_t batch_axis_count_{};
};

} // namespace markov
