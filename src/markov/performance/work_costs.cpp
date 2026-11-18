#include "markov/performance/work_costs.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

#include "markov/graph/graph.hpp"
#include "markov/performance/work_features.hpp"
#include "markov/templates/library.hpp"

namespace markov {

CostGap::CostGap(nlohmann::json details)
    : std::runtime_error("no qualified service cost at " + details.at("site").get<std::string>()),
      details_(std::move(details)) {}

WorkCosts::Curve::Curve(const nlohmann::json & model) {
    for (const auto & [name, range] : model.at("bounds").get_ref<const nlohmann::json::object_t &>()) {
        const auto & values = range.get_ref<const nlohmann::json::array_t &>();
        if (values.size() != 2) throw std::invalid_argument("cost feature bounds require two coordinates");
        const auto lower = values[0].get<double>();
        const auto upper = values[1].get<double>();
        if (!std::isfinite(lower) || !std::isfinite(upper) || lower > upper)
            throw std::invalid_argument("cost feature bounds must be finite and ordered");
        bounds.emplace(name, Bound{ lower, upper });
    }
    if (!model.at("axis").is_null()) axis = model.at("axis").get<std::string>();
    for (const auto & row : model.at("knots").get_ref<const nlohmann::json::array_t &>()) {
        const auto & values = row.get_ref<const nlohmann::json::array_t &>();
        if (values.size() != 2 || !values[1].is_number_integer())
            throw std::invalid_argument("cost knot requires a coordinate and integer service nanoseconds");
        const auto coordinate = values[0].get<double>();
        const auto ns = values[1].get<std::int64_t>();
        if (!std::isfinite(coordinate) || ns < 0 || (!points.empty() && coordinate <= points.back().coordinate))
            throw std::invalid_argument("cost knots require increasing coordinates and nonnegative costs");
        points.push_back({ coordinate, ns });
    }

    // Interpolation must cover exactly the declared axis range. Constant models also retain feature bounds.
    if (axis) {
        const auto bound = bounds.find(*axis);
        if (points.size() < 2 || bound == bounds.end() || points.front().coordinate != bound->second.lower
            || points.back().coordinate != bound->second.upper)
            throw std::invalid_argument("cost knots do not span their declared feature axis");
    } else if (points.size() != 1) {
        throw std::invalid_argument("a constant cost model requires one knot");
    }
}

ServiceDuration WorkCosts::Curve::estimate(const WorkFeatures & features) const {
    double coordinate = 0;
    for (const auto & [name, bound] : bounds) {
        const auto value = features.value(name);
        if (!value || !std::isfinite(*value) || *value < bound.lower || *value > bound.upper) return {};
        if (axis && name == *axis) coordinate = *value;
    }
    if (!axis) return points.front().ns;
    const auto right = std::ranges::lower_bound(points, coordinate, {}, &Point::coordinate);
    if (right->coordinate == coordinate) return right->ns;
    const auto & left = *std::prev(right);
    const long double fraction = (static_cast<long double>(coordinate) - left.coordinate)
        / (static_cast<long double>(right->coordinate) - left.coordinate);
    const long double service = left.ns + fraction * (static_cast<long double>(right->ns) - left.ns);
    return static_cast<std::int64_t>(std::nearbyint(service));
}

WorkCosts::WorkCosts(const TemplateLibrary & library, const nlohmann::json & asset) {
    if (asset.at("domain") != library.domain)
        throw std::invalid_argument("cost asset and selected templates use different implementation domains");
    const auto & definitions = asset.at("definitions").get_ref<const nlohmann::json::array_t &>();
    std::map<nlohmann::json, std::size_t> known;
    models_.resize(definitions.size());
    for (std::size_t index = 0; index < definitions.size(); ++index) {
        if (!known.emplace(definitions[index], index).second)
            throw std::invalid_argument("cost asset repeats a template definition");
        models_[index].resize(definitions[index].at("work").get_ref<const nlohmann::json::array_t &>().size());
    }
    for (const auto & model : asset.at("models").get_ref<const nlohmann::json::array_t &>()) {
        const auto definition = model.at("definition").get<std::size_t>();
        const auto interval = model.at("interval").get<std::size_t>();
        auto & phases = models_.at(definition).at(interval);
        if (!phases.try_emplace(model.at("phase").get<std::string>(), model).second)
            throw std::invalid_argument("cost asset repeats a work interval and phase");
    }

    // Source and target library indices are local to their assets. Compare canonical definitions only at load time.
    definitions_.reserve(library.definitions.size());
    for (const auto & definition : library.definitions) {
        const auto source = known.find(definition.document());
        definitions_.push_back(source == known.end() ? std::nullopt : std::optional(source->second));
    }
}

const WorkCosts::Curve * WorkCosts::curve(const TemplateInterval & origin, std::string_view phase) const {
    const auto definition = definitions_.at(origin.definition);
    if (!definition) return nullptr;
    const auto & phases = models_[*definition].at(origin.interval);
    const auto exact = phases.find(phase);
    return exact == phases.end() ? nullptr : &exact->second;
}

GraphTiming WorkCosts::timing(const CoarseGraph & graph, std::span<const TemplateInterval> origins) const {
    if (origins.size() != graph.work.size()) throw std::logic_error("cost queries require one origin per instantiated work interval");
    std::vector<std::optional<WorkFeatures>> features(graph.scopes.size());
    GraphTiming result;
    result.work_ns.reserve(graph.work.size());
    for (std::size_t index = 0; index < graph.work.size(); ++index) {
        const auto & work = graph.work[index];
        const auto & origin = origins[index];
        const auto & scope = graph.scopes[work.scope];
        auto & geometry = features[work.scope];
        if (!geometry) geometry.emplace(scope);
        const auto * candidate = curve(origin, geometry->phase());
        const auto estimate = candidate ? candidate->estimate(*geometry) : std::nullopt;
        if (!estimate) {
            auto bounds = nlohmann::json::object();
            if (candidate) {
                for (const auto & [name, bound] : candidate->bounds) bounds[name] = { bound.lower, bound.upper };
            }
            throw CostGap({
                { "status", candidate ? "outside_sample_ranges" : "unseen_work" },
                { "action_site", graph.scopes.front().info.site },
                { "site", scope.info.site }, { "implementation", scope.info.implementation },
                { "scope", scope.info.role }, { "work", work_name(work.kind) }, { "resource", graph.resources[work.resource].role },
                { "definition", origin.definition }, { "interval", origin.interval },
                { "features", geometry->document() }, { "sample_bounds", std::move(bounds) },
            });
        }
        result.work_ns.push_back(estimate);
    }
    return result;
}

} // namespace markov
