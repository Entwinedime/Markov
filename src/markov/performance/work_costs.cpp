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

WorkCosts::WorkCosts(const TemplateLibrary & library, const nlohmann::json & asset) : library_(library) {
    definitions_.resize(library.definitions.size());
    const nlohmann::json * selected = nullptr;
    for (const auto & domain : asset.at("domains").get_ref<const nlohmann::json::array_t &>()) {
        if (domain.at("domain") != library.domain) continue;
        if (selected) throw std::invalid_argument("cost asset repeats the selected implementation domain");
        selected = &domain;
    }
    if (!selected) return;

    const auto & definitions = selected->at("definitions").get_ref<const nlohmann::json::array_t &>();
    std::map<nlohmann::json, std::size_t> known;
    models_.resize(definitions.size());
    for (std::size_t index = 0; index < definitions.size(); ++index) {
        const auto & definition = definitions[index];
        const auto & work = definition.at("work").get_ref<const nlohmann::json::array_t &>();
        if (definition.contains("external")) {
            if (work.size() != 1) throw std::invalid_argument("external cost definition must declare one work interval");
            const auto kind = work_kind(work.front().at("kind").get<std::string>());
            if (!external_.emplace(definition.at("external"), std::pair{ index, kind }).second)
                throw std::invalid_argument("cost asset repeats an external work definition");
        } else if (!known.emplace(definition, index).second) {
            throw std::invalid_argument("cost asset repeats a template definition");
        }
        models_[index].resize(work.size());
    }
    for (const auto & model : selected->at("models").get_ref<const nlohmann::json::array_t &>()) {
        const auto definition = model.at("definition").get<std::size_t>();
        const auto interval = model.at("interval").get<std::size_t>();
        auto & phases = models_.at(definition).at(interval);
        if (!phases.try_emplace(model.at("phase").get<std::string>(), model).second)
            throw std::invalid_argument("cost asset repeats a work interval and phase");
    }

    // Source and target library indices are local to their assets. Compare canonical definitions only at load time.
    for (std::size_t index = 0; index < library.definitions.size(); ++index) {
        const auto source = known.find(library.definitions[index].document());
        if (source != known.end()) definitions_[index] = source->second;
    }
}

const WorkCosts::Curve * WorkCosts::curve(std::optional<std::size_t> definition, std::size_t interval, std::string_view phase) const {
    if (!definition) return nullptr;
    const auto & phases = models_[*definition].at(interval);
    const auto exact = phases.find(phase);
    return exact == phases.end() ? nullptr : &exact->second;
}

CostGap WorkCosts::gap(const Curve * candidate, const WorkFeatures & features, nlohmann::json details) const {
    auto bounds = nlohmann::json::object();
    if (candidate) {
        for (const auto & [name, bound] : candidate->bounds) bounds[name] = { bound.lower, bound.upper };
    }
    details["status"] = candidate ? "outside_sample_ranges" : "unseen_work";
    details["domain"] = library_.domain;
    details["features"] = features.document();
    details["sample_bounds"] = std::move(bounds);
    return CostGap(std::move(details));
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
        const auto * candidate = curve(definitions_.at(origin.definition), origin.interval, geometry->phase());
        const auto estimate = candidate ? candidate->estimate(*geometry) : std::nullopt;
        if (!estimate) {
            throw gap(candidate, *geometry, {
                { "action_site", graph.scopes.front().info.site },
                { "site", scope.info.site }, { "implementation", scope.info.implementation },
                { "scope", scope.info.role }, { "work", work_name(work.kind) }, { "resource", graph.resources[work.resource].role },
                { "definition", library_.definitions.at(origin.definition).document() }, { "interval", origin.interval },
            });
        }
        result.work_ns.push_back(estimate);
    }
    return result;
}

std::int64_t WorkCosts::external(const nlohmann::json & identity, WorkKind kind, const WorkFeatures & features) const {
    const auto found = external_.find(identity);
    if (found != external_.end() && found->second.second != kind)
        throw std::invalid_argument("external cost definition changes the declared work kind");
    const auto * candidate = found == external_.end() ? nullptr : curve(found->second.first, 0, features.phase());
    if (const auto estimate = candidate ? candidate->estimate(features) : std::nullopt) return *estimate;

    throw gap(candidate, features, {{ "site", identity.at("operation") }, { "work", work_name(kind) },
                                   { "definition", {{ "external", identity }, { "work", {{{ "kind", work_name(kind) }}} }} },
                                   { "interval", 0 }});
}

} // namespace markov
