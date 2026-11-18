#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "markov/graph/graph.hpp"

namespace markov {

class WorkFeatures {
public:
    explicit WorkFeatures(const GraphScope & scope);

    std::string_view phase() const { return phase_; }
    nlohmann::json document() const;

private:
    Parameters parameters_;
    std::string_view phase_;
    double members_ = 0;
    double query_tokens_ = 0;
    double context_tokens_ = 0;
    double max_context_ = 0;
    double attention_pairs_ = 0;
};

} // namespace markov
