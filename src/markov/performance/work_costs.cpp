#include "markov/performance/work_costs.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <ranges>
#include <utility>

#include "markov/graph/graph.hpp"
#include "markov/performance/work_features.hpp"
#include "markov/templates/library.hpp"

namespace markov {

namespace {

class RecordedFeatures {
public:
    explicit RecordedFeatures(const nlohmann::json & document)
        : phase_(document.at("phase").get_ref<const std::string &>()),
          axes_(document.at("axes").get_ref<const nlohmann::json::object_t &>()) {}

    std::string_view phase() const { return phase_; }

    std::optional<double> value(const std::string & axis) const {
        const auto found = axes_.find(axis);
        if (found == axes_.end()) return {};
        return found->second.get<double>();
    }

private:
    std::string_view phase_;
    const nlohmann::json::object_t & axes_;
};

} // namespace

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

template<class Features>
ServiceDuration WorkCosts::Curve::estimate(const Features & features) const {
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

template<class Definitions>
WorkCosts::WorkCosts(nlohmann::json domain, const nlohmann::json & asset, const Definitions & definitions)
    : domain_(std::move(domain)), model_indices_(definitions.size()) {
    const nlohmann::json * selected = nullptr;
    for (const auto & candidate : asset.at("domains").get_ref<const nlohmann::json::array_t &>()) {
        if (candidate.at("domain") != domain_) continue;
        if (selected) throw std::invalid_argument("cost asset repeats the selected implementation domain");
        selected = &candidate;
    }
    if (!selected) return;

    const auto & measured = selected->at("definitions").get_ref<const nlohmann::json::array_t &>();
    std::map<nlohmann::json, std::size_t> known;
    models_.resize(measured.size());
    for (std::size_t index = 0; index < measured.size(); ++index) {
        const auto & definition = measured[index];
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

    // Work definitions may be shared; the models always come from the selected execution domain.
    std::size_t index = 0;
    for (const nlohmann::json & definition : definitions) {
        auto & model = model_indices_[index++];
        if (definition.contains("external")) {
            const auto kind = work_kind(definition.at("work").at(0).at("kind").get<std::string>());
            model = external_index(definition.at("external"), kind);
        } else if (const auto source = known.find(definition); source != known.end()) {
            model = source->second;
        }
    }
}

WorkCosts::WorkCosts(nlohmann::json domain, std::span<const TemplateDefinition> definitions, const nlohmann::json & asset)
    : WorkCosts(std::move(domain), asset, definitions | std::views::transform([](const auto & definition) {
        return definition.document();
    })) {
    definitions_ = definitions;
}

std::optional<std::size_t> WorkCosts::external_index(const nlohmann::json & identity, WorkKind kind) const {
    const auto found = external_.find(identity);
    if (found == external_.end()) return {};
    if (found->second.second != kind)
        throw std::invalid_argument("external cost definition changes the declared work kind");
    return found->second.first;
}

template<class Features>
WorkCosts::Estimate WorkCosts::estimate(std::optional<std::size_t> definition, std::size_t interval, const Features & features) const {
    if (!definition) return {};
    const auto & phases = models_[*definition].at(interval);
    const auto match = phases.find(features.phase());
    if (match == phases.end()) return {};
    return { &match->second, match->second.estimate(features) };
}

std::string_view WorkCosts::Estimate::status() const {
    if (service_ns) return "covered";
    return model ? "outside_sample_ranges" : "unseen_work";
}

CostGap WorkCosts::gap(const Estimate & estimate, const WorkFeatures & features, nlohmann::json details) const {
    auto bounds = nlohmann::json::object();
    if (estimate.model) {
        for (const auto & [name, bound] : estimate.model->bounds) bounds[name] = { bound.lower, bound.upper };
    }
    details["status"] = estimate.status();
    details["domain"] = domain_;
    details["features"] = features.document();
    details["sample_bounds"] = std::move(bounds);
    return CostGap(std::move(details));
}

GraphTiming WorkCosts::timing(const CoarseGraph & graph, std::span<const TemplateInterval> origins, nlohmann::json * first_gap) const {
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
        const auto cost = estimate(model_indices_.at(origin.definition), origin.interval, *geometry);
        if (!cost.service_ns && (!first_gap || first_gap->is_null())) {
            auto missing = gap(cost, *geometry, {
                { "action_site", graph.scopes.front().info.site },
                { "site", scope.info.site }, { "implementation", scope.info.implementation },
                { "scope", scope.info.role }, { "work", work_name(work.kind) }, { "resource", graph.resources[work.resource].role },
                { "definition", definitions_[origin.definition].document() }, { "interval", origin.interval },
            });
            if (!first_gap) throw missing;
            *first_gap = missing.details();
            (*first_gap)["work_index"] = index;
        }
        result.work_ns.push_back(cost.service_ns);
    }
    return result;
}

std::int64_t WorkCosts::external(const nlohmann::json & identity, WorkKind kind, const WorkFeatures & features) const {
    const auto cost = estimate(external_index(identity, kind), 0, features);
    if (cost.service_ns) return *cost.service_ns;

    throw gap(cost, features, {{ "site", identity.at("operation") }, { "work", work_name(kind) },
                                   { "definition", {{ "external", identity }, { "work", {{{ "kind", work_name(kind) }}} }} },
                                   { "interval", 0 }});
}

nlohmann::json WorkCosts::evaluate(const nlohmann::json & groups, const nlohmann::json & asset) {
    auto results = nlohmann::json::array();
    for (const auto & group : groups.get_ref<const nlohmann::json::array_t &>()) {
        const auto & definitions = group.at("definitions").get_ref<const nlohmann::json::array_t &>();
        const WorkCosts costs(group.at("domain"), asset, definitions);
        auto estimates = nlohmann::json::array();
        for (const auto & point : group.at("points").get_ref<const nlohmann::json::array_t &>()) {
            if (!point.is_array() || point.size() != 3)
                throw std::invalid_argument("cost point requires a definition, interval, and features");
            const auto definition = point[0].get<std::size_t>();
            const auto interval = point[1].get<std::size_t>();
            definitions.at(definition).at("work").at(interval);

            const auto estimate = costs.estimate(costs.model_indices_[definition], interval, RecordedFeatures(point[2]));
            const auto duration = estimate.service_ns ? nlohmann::json(*estimate.service_ns) : nlohmann::json();
            estimates.push_back(nlohmann::json::array({ estimate.status(), duration }));
        }
        results.push_back(std::move(estimates));
    }
    return results;
}

} // namespace markov
