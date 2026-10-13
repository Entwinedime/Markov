#include "markov/performance/cost_statistics.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <numeric>

namespace markov {

void CostDistribution::observe(std::int64_t duration_ns) { values_.push_back(duration_ns); }

nlohmann::json CostDistribution::describe() const {
    auto ordered = values_;
    std::ranges::sort(ordered);
    const auto middle = ordered.size() / 2;
    const auto median = ordered.size() % 2 == 0 ? std::midpoint(ordered[middle - 1], ordered[middle]) : ordered[middle];
    return {
        { "estimate_ns",                              median },
        {       "count",                      ordered.size() },
        {    "range_ns", { ordered.front(), ordered.back() } }
    };
}

void CostErrors::observe(std::int64_t estimate, std::int64_t observed) {
    const auto error = estimate - observed;
    const auto absolute = error < 0 ? -error : error;
    ++count_;
    observed_ns_ += observed;
    absolute_ns_ += absolute;
    signed_ns_ += error;
    maximum_ns_ = std::max(maximum_ns_, absolute);
}

nlohmann::json CostErrors::describe() const {
    using Json = nlohmann::json;
    return {
        {                              "count",                                                                count_ },
        {             "mean_absolute_error_ns",             count_ == 0 ? Json(nullptr) : Json(absolute_ns_ / count_) },
        {               "mean_signed_error_ns",               count_ == 0 ? Json(nullptr) : Json(signed_ns_ / count_) },
        { "weighted_absolute_percentage_error", observed_ns_ == 0 ? Json(nullptr) : Json(absolute_ns_ / observed_ns_) },
        {              "max_absolute_error_ns",                       count_ == 0 ? Json(nullptr) : Json(maximum_ns_) }
    };
}

} // namespace markov
