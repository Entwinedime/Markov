#pragma once

#include "markov/trace_graph/structure/operation.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
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
    size_t queries = 0, outside_range_queries = 0, unseen_combination_queries = 0;
    uint64_t queried_duration_sum_us = 0;
};

/** Per-execution model provider with compact query provenance. These counters
 * count queries, not emitted nodes: generators may reuse one result many
 * times. Duration sums are neither executed service totals nor E2E coverage.
 * Owns no graph/state and cannot change an operation's work or dependencies.
 * Models are already validated at the input boundary; their storage must
 * remain immutable and outlive the provider. Missing evidence is a cost gap,
 * never permission to scale source costs or change an execution branch.
 */
class OperatorCostProvider {
public:
    using QueryKey = std::tuple<std::string, std::string, std::string, std::string>;
    explicit OperatorCostProvider(const std::vector<OperatorModel> & models);
    [[nodiscard]] OperatorEstimate estimate(const structure::OperationWork & work);
    /** Query already-determined work in order. Any missing costs produce one
     * combined error; no partial vector or fabricated duration is returned.
     * This does not expand future timing-dependent execution branches. */
    [[nodiscard]] std::vector<OperatorEstimate> estimate_all(std::span<const structure::OperationWork> work);
    [[nodiscard]] const std::map<QueryKey, OperatorQuerySummary> & queries() const { return queries_; }
    /** Optional host-side query timing/point accounting; never a model input. */
    nlohmann::json performance() const;

private:
    using ModelKey = std::tuple<std::string_view, std::string_view, std::string_view>;
    std::map<ModelKey, const OperatorModel *> models_;
    std::map<QueryKey, OperatorQuerySummary> queries_;
    bool measure_queries_ = false;
    double query_wall_seconds_ = 0;
    size_t measured_queries_ = 0;
    std::set<std::pair<ModelKey, std::map<std::string, double>>> measured_points_;
};

} // namespace markov::trace_graph::cost
