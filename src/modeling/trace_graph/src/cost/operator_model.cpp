#include "markov/trace_graph/cost/operator_model.hpp"

#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/cost/missing_evidence.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <set>
#include <utility>

namespace markov::trace_graph::cost {

OperatorCostProvider::OperatorCostProvider(const std::vector<OperatorModel> & models) {
    for (const auto & model : models) models_.emplace(ModelKey{ model.operation, model.implementation, model.timing_scope }, &model);
}

std::vector<OperatorEstimate> OperatorCostProvider::estimate_all(std::span<const structure::OperationWork> work) {
    std::vector<OperatorEstimate> result;
    result.reserve(work.size());
    auto missing = nlohmann::json::array();
    std::set<std::string> identities;
    for (const auto & operation : work) {
        try {
            result.push_back(estimate(operation));
        }
        catch (const MissingCostEvidence & error) {
            for (const auto & requirement : error.requirements)
                if (identities.insert(requirement.dump()).second) missing.push_back(requirement);
        }
    }

    if (!missing.empty()) throw MissingCostEvidence(std::move(missing));
    return result;
}

OperatorEstimate OperatorCostProvider::estimate(const structure::OperationWork & work) {
    const auto describe = [&] {
        return nlohmann::json{
            {      "operation",      work.operation },
            { "implementation", work.implementation },
            {   "timing_scope",   work.timing_scope },
            {       "features",       work.features }
        };
    };
    const auto found = models_.find(ModelKey{ work.operation, work.implementation, work.timing_scope });
    if (found == models_.end()) {
        auto coordinates = describe();
        auto scopes = nlohmann::json::array();
        for (const auto & [key, model] : models_)
            if (model->operation == work.operation && model->implementation == work.implementation) scopes.push_back(model->timing_scope);
        coordinates["available_timing_scopes"] = std::move(scopes);
        throw cost::MissingCostEvidence(work.component + "/operator",
                                        coordinates,
                                        "No applicable database model for " + work.operation + ":" + work.implementation + ":" + work.timing_scope);
    }

    const auto & model = *found->second;
    bool outside = false;
    // Input admission proves that regression/curve coordinates are included
    // in feature_ranges. Establish this query's work point once, then evaluate
    // the response and joint coverage from that same description.
    std::map<std::string, double> point;
    for (const auto & [name, range] : model.feature_ranges) {
        const auto feature = work.features.find(name);
        if (feature == work.features.end()) {
            auto coordinates = describe();
            coordinates["missing_feature"] = name;
            throw cost::MissingCostEvidence(work.component + "/feature", coordinates, "Missing declared operator feature: " + name);
        }

        point.emplace(name, feature->second);
        const bool feature_outside = feature->second < range.first || feature->second > range.second;
        outside |= feature_outside;
        if (feature_outside && !model.allow_extrapolation && std::ranges::find(model.extrapolation_features, name) == model.extrapolation_features.end()) {
            auto coordinates = describe();
            coordinates["measured_feature_ranges"] = model.feature_ranges;
            throw cost::MissingCostEvidence(work.component + "/coverage", coordinates, "Operator work is outside measured coverage");
        }
    }

    double value = model.intercept_us;
    for (const auto & [name, coefficient] : model.coefficients) value += coefficient * point.at(name);

    if (model.form == "piecewise_linear") {
        const auto coordinate = point.at(model.coordinate);
        auto right = std::ranges::lower_bound(model.points, coordinate, {}, [](const auto & point) { return point.first; });
        if (right == model.points.begin()) ++right;
        else if (right == model.points.end()) right = std::prev(model.points.end());
        const auto left = std::prev(right);
        value = left->second + (coordinate - left->first) * (right->second - left->second) / (right->first - left->first);
    }

    const auto rounded = core::truncate_to_u64(value + 0.5);
    if (!std::isfinite(value) || value < 0.0 || !rounded)
        throw cost::MissingCostEvidence(work.component + "/model", describe(), "Database operator cost is not a finite non-negative duration");

    const bool unseen = std::ranges::find(model.measured_feature_points, point) == model.measured_feature_points.end();
    const OperatorEstimate result{ *rounded, outside, unseen };
    auto & summary = queries_[{ work.component, work.operation, work.implementation, work.timing_scope }];
    ++summary.queries;
    summary.outside_range_queries += result.outside_measured_range;
    summary.unseen_combination_queries += result.unseen_feature_combination;
    summary.queried_duration_sum_us = core::checked_add_u64(summary.queried_duration_sum_us, result.duration_us, "Operator query summary overflow");
    return result;
}

} // namespace markov::trace_graph::cost
