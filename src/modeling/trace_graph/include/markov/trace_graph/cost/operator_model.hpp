#pragma once

#include "markov/trace_graph/structure/operation.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace markov::trace_graph::cost {

/** Offline-trained operator response. Inference does not read trace databases.
 * Linear features and duration are in the units declared in OperationWork/us.
 * Constant models have no variable coefficients. Coverage describes measured
 * ranges, not a promise that all combinations within those ranges were seen.
 */
struct OperatorModel {
    std::string operation;
    std::string implementation;
    std::string timing_scope;
    std::string form = "linear";
    std::string coordinate;
    std::vector<std::pair<double, double>> points;
    double intercept_us = 0.0;
    std::map<std::string, double> coefficients;
    std::map<std::string, std::pair<double, double>> feature_ranges;
    std::vector<std::map<std::string, double>> measured_feature_points;
    bool allow_extrapolation = false;
    // Named coordinates may extrapolate while all other measured ranges stay
    // hard constraints. The bool above explicitly permits every coordinate.
    std::vector<std::string> extrapolation_features;
};

struct OperatorEstimate {
    uint64_t duration_us = 0;
    bool outside_measured_range = false;
    bool unseen_feature_combination = false;
};

struct OperatorQuerySummary {
    size_t queries = 0, outside_range_queries = 0, unseen_combination_queries = 0, cache_hits = 0;
    uint64_t queried_duration_sum_us = 0;
};

using OperatorPoint = std::vector<double>;

struct OperatorPointHash {
    size_t operator()(const OperatorPoint & point) const;
};

/** Immutable responses and numeric coverage indexes, shared by all targets. */
class OperatorModels {
public:
    struct Entry {
        size_t id;
        std::vector<std::string> coordinates;
        std::vector<std::pair<size_t, double>> coefficients;
        std::vector<bool> extrapolatable;
        std::unordered_set<OperatorPoint, OperatorPointHash> measured;
        size_t curve_coordinate = 0;
    };

    explicit OperatorModels(std::vector<OperatorModel> models);
    OperatorModels(const OperatorModels &) = delete;
    OperatorModels & operator=(const OperatorModels &) = delete;
    const Entry * find(const structure::OperationWork & work) const;
    const OperatorModel & definition(size_t id) const { return definitions_.at(id); }
    size_t size() const { return definitions_.size(); }

private:
    using ModelKey = std::tuple<std::string_view, std::string_view, std::string_view>;
    std::vector<OperatorModel> definitions_;
    std::vector<Entry> entries_;
    std::map<ModelKey, size_t> index_;
};

/** One target owns its response cache and query counts. Only immutable model
 * work coordinates enter the cache; clocks, queues and policies never do. */
class OperatorCostProvider {
public:
    using QueryKey = std::tuple<std::string, std::string, std::string, std::string>;
    explicit OperatorCostProvider(std::shared_ptr<const OperatorModels> models);
    OperatorEstimate estimate(const structure::OperationWork & work);
    std::vector<OperatorEstimate> estimate_all(std::span<const structure::OperationWork> work);
    std::map<QueryKey, OperatorQuerySummary> queries() const;
    nlohmann::json performance() const;

private:
    std::shared_ptr<const OperatorModels> models_;
    std::vector<std::unordered_map<OperatorPoint, OperatorEstimate, OperatorPointHash>> responses_;
    std::map<std::pair<std::string, size_t>, OperatorQuerySummary> queries_;
    bool measure_queries_ = false;
    double query_wall_seconds_ = 0;
    size_t measured_queries_ = 0, cache_hits_ = 0;
};

} // namespace markov::trace_graph::cost
