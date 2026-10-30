#include "markov/trace_graph/cost/operator_model.hpp"

#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/cost/missing_evidence.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <set>
#include <utility>

namespace markov::trace_graph::cost {

size_t OperatorPointHash::operator()(const OperatorPoint & point) const {
    size_t result = 0;
    for (const auto value : point) result ^= std::hash<double>{}(value) + static_cast<size_t>(0x9e37'79b9'7f4a'7c15ULL) + (result << 6U) + (result >> 2U);

    return result;
}

OperatorModels::OperatorModels(std::vector<OperatorModel> models) : definitions_(std::move(models)) {
    entries_.reserve(definitions_.size());
    for (size_t id = 0; id < definitions_.size(); ++id) {
        const auto & model = definitions_[id];
        Entry entry{ .id = id };
        for (const auto & [name, range] : model.feature_ranges) {
            entry.coordinates.push_back(name);
            entry.extrapolatable.push_back(model.allow_extrapolation
                                           || std::ranges::find(model.extrapolation_features, name) != model.extrapolation_features.end());
        }

        for (const auto & [name, value] : model.coefficients) {
            const auto at = std::ranges::lower_bound(entry.coordinates, name);
            entry.coefficients.emplace_back(static_cast<size_t>(at - entry.coordinates.begin()), value);
        }

        if (model.form == "piecewise_linear")
            entry.curve_coordinate = static_cast<size_t>(std::ranges::lower_bound(entry.coordinates, model.coordinate) - entry.coordinates.begin());

        for (const auto & observed : model.measured_feature_points) {
            OperatorPoint point;
            point.reserve(entry.coordinates.size());
            for (const auto & name : entry.coordinates) point.push_back(observed.at(name));
            entry.measured.insert(std::move(point));
        }

        index_.emplace(ModelKey{ model.operation, model.implementation, model.timing_scope }, id);
        entries_.push_back(std::move(entry));
    }
}

const OperatorModels::Entry * OperatorModels::find(const structure::OperationWork & work) const {
    const auto found = index_.find(ModelKey{ work.operation, work.implementation, work.timing_scope });
    return found == index_.end() ? nullptr : &entries_[found->second];
}

OperatorCostProvider::OperatorCostProvider(std::shared_ptr<const OperatorModels> models)
    : models_(std::move(models)),
      responses_(models_ ? models_->size() : 0) {
    const auto requested = std::getenv("TRACE_GRAPH_QUERY_METRICS");
    measure_queries_ = requested && std::string_view(requested) == "1";
}

std::map<OperatorCostProvider::QueryKey, OperatorQuerySummary> OperatorCostProvider::queries() const {
    std::map<QueryKey, OperatorQuerySummary> result;
    for (const auto & [key, summary] : queries_) {
        const auto & model = models_->definition(key.second);
        result.emplace(QueryKey{ key.first, model.operation, model.implementation, model.timing_scope }, summary);
    }
    return result;
}

nlohmann::json OperatorCostProvider::performance() const {
    size_t distinct = 0;
    for (const auto & responses : responses_) distinct += responses.size();
    return {
        {   "detailed_metrics_enabled",                                                                 measure_queries_ },
        {         "query_wall_seconds", measure_queries_ ? nlohmann::json(query_wall_seconds_) : nlohmann::json(nullptr) },
        {                "query_count",                                                                measured_queries_ },
        { "distinct_model_work_points",                                                                         distinct },
        {        "response_cache_hits",                                                                      cache_hits_ },
        {                      "scope", "successful pure model queries; cache hits retain query and coverage accounting" }
    };
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
    const auto started = measure_queries_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const auto describe = [&] {
        return nlohmann::json{
            {      "operation",      work.operation },
            { "implementation", work.implementation },
            {   "timing_scope",   work.timing_scope },
            {       "features",       work.features }
        };
    };
    const auto entry = models_ ? models_->find(work) : nullptr;
    if (!entry) {
        auto coordinates = describe();
        auto scopes = nlohmann::json::array();
        if (models_)
            for (size_t id = 0; id < models_->size(); ++id) {
                const auto & model = models_->definition(id);
                if (model.operation == work.operation && model.implementation == work.implementation) scopes.push_back(model.timing_scope);
            }
        coordinates["available_timing_scopes"] = std::move(scopes);
        throw MissingCostEvidence(work.component + "/operator",
                                  coordinates,
                                  "No applicable database model for " + work.operation + ":" + work.implementation + ":" + work.timing_scope);
    }

    const auto & model = models_->definition(entry->id);
    OperatorPoint point;
    point.reserve(entry->coordinates.size());
    for (const auto & name : entry->coordinates) {
        const auto feature = work.features.find(name);
        if (feature == work.features.end()) {
            auto coordinates = describe();
            coordinates["missing_feature"] = name;
            throw MissingCostEvidence(work.component + "/feature", coordinates, "Missing declared operator feature: " + std::string(name));
        }
        point.push_back(feature->second);
    }

    auto & responses = responses_[entry->id];
    const auto cached = responses.find(point);
    const bool hit = cached != responses.end();
    OperatorEstimate result;
    if (hit) result = cached->second;
    else {
        bool outside = false;
        for (size_t i = 0; i < point.size(); ++i) {
            const auto range = model.feature_ranges.at(entry->coordinates[i]);
            const bool feature_outside = point[i] < range.first || point[i] > range.second;
            outside |= feature_outside;
            if (feature_outside && !entry->extrapolatable[i]) {
                auto coordinates = describe();
                coordinates["measured_feature_ranges"] = model.feature_ranges;
                throw MissingCostEvidence(work.component + "/coverage", coordinates, "Operator work is outside measured coverage");
            }
        }

        double value = model.intercept_us;
        for (const auto & [coordinate, coefficient] : entry->coefficients) value += coefficient * point[coordinate];
        if (model.form == "piecewise_linear") {
            const auto coordinate = point[entry->curve_coordinate];
            auto right = std::ranges::lower_bound(model.points, coordinate, {}, [](const auto & anchor) { return anchor.first; });
            if (right == model.points.begin()) ++right;
            else if (right == model.points.end()) right = std::prev(model.points.end());
            const auto left = std::prev(right);
            value = left->second + (coordinate - left->first) * (right->second - left->second) / (right->first - left->first);
        }

        const auto rounded = core::truncate_to_u64(value + 0.5);
        if (!std::isfinite(value) || value < 0.0 || !rounded)
            throw MissingCostEvidence(work.component + "/model", describe(), "Database operator cost is not a finite non-negative duration");
        result = { *rounded, outside, !entry->measured.contains(point) };
        responses.emplace(std::move(point), result);
    }

    auto & summary = queries_[{ work.component, entry->id }];
    ++summary.queries;
    summary.cache_hits += hit;
    summary.outside_range_queries += result.outside_measured_range;
    summary.unseen_combination_queries += result.unseen_feature_combination;
    summary.queried_duration_sum_us = core::checked_add_u64(summary.queried_duration_sum_us, result.duration_us, "Operator query summary overflow");
    ++measured_queries_;
    cache_hits_ += hit;
    if (measure_queries_) query_wall_seconds_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return result;
}

} // namespace markov::trace_graph::cost
