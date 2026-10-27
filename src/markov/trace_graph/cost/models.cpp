#include "markov/trace_graph/cost/models.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace markov::trace_graph::cost {
namespace {

long double evaluate(const Affine & response, const planning::Features & features) {
    long double result = response.intercept_ns;
    for (const auto & [name, coefficient] : response.coefficients_ns) result += static_cast<long double>(coefficient) * features.at(name);
    return result;
}

long double evaluate(const Formula & response, const planning::Features & features) {
    if (const auto * affine = std::get_if<Affine>(&response)) return evaluate(*affine, features);

    const auto & curve = std::get<CoefficientCurve>(response);
    const auto coordinate = features.at(curve.feature);
    const auto right = std::ranges::upper_bound(curve.points, coordinate, {}, &CoefficientPoint::coordinate);
    if (right == curve.points.end()) return evaluate(curve.points.back().response, features);

    // Intake requires the coordinate domain to lie within the measured curve.
    // Interpolate response coefficients, never clamp an unsupported work point.
    const auto & left = *(right - 1);
    const auto position = (coordinate - left.coordinate) / (right->coordinate - left.coordinate);
    const auto lower = evaluate(left.response, features);
    return lower + position * (evaluate(right->response, features) - lower);
}

} // namespace

Models::Models(std::vector<Model> models) : models_(std::move(models)) {
    for (std::size_t id = 0; id < models_.size(); ++id) {
        if (!index_.emplace(models_[id].kind, id).second) throw std::runtime_error("Multiple selected cost models for " + models_[id].kind.operation);
    }
}

Response Models::query(const planning::Work & work) const {
    const auto found = index_.find(work.kind);
    if (found == index_.end()) return { Status::MissingModel, std::nullopt, std::nullopt, {}, std::nullopt };

    const auto id = found->second;
    const auto & model = models_[id];
    std::vector<std::string> missing, outside;
    std::vector<double> coordinates;
    bool extrapolation_allowed = true;
    for (const auto & [name, range] : model.ranges) {
        const auto feature = work.features.find(name);
        if (feature == work.features.end()) {
            missing.push_back(name);
            continue;
        }

        const auto value = feature->second;
        coordinates.push_back(value);
        if (value < range.lower || value > range.upper) {
            outside.push_back(name);
            extrapolation_allowed &= model.extrapolation_features.contains(name);
        }
    }
    if (!missing.empty()) return { Status::MissingFeatures, std::nullopt, std::nullopt, std::move(missing), id };
    if (!outside.empty() && !extrapolation_allowed) return { Status::OutsideDomain, std::nullopt, std::nullopt, std::move(outside), id };

    const bool measured = model.measured_points.contains(coordinates);
    const auto coverage = !outside.empty() ? Coverage::Extrapolated : measured ? Coverage::Measured : Coverage::NewCombination;
    if (coverage == Coverage::NewCombination && !model.allow_new_combinations) return { Status::UnmeasuredCombination, std::nullopt, coverage, {}, id };

    const auto duration = evaluate(model.response, work.features);

    // A regression may have a negative intercept; its physical response still
    // must fit the nonnegative integer-ns contract. Never clamp it to zero.
    if (!std::isfinite(duration) || duration < 0 || duration >= static_cast<long double>(std::numeric_limits<graph::Time>::max()))
        return { Status::InvalidResponse, std::nullopt, coverage, std::move(outside), id };

    return { Status::Ready, static_cast<graph::Time>(std::round(duration)), coverage, std::move(outside), id };
}

} // namespace markov::trace_graph::cost
