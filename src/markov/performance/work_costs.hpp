#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "markov/execution/timing.hpp"

namespace markov {

class CoarseGraph;
class WorkFeatures;
enum class WorkKind;
struct TemplateDefinition;
struct TemplateInterval;

class CostGap final : public std::runtime_error {
public:
    explicit CostGap(nlohmann::json details);
    const nlohmann::json & details() const { return details_; }

private:
    nlohmann::json details_;
};

class WorkCosts {
public:
    WorkCosts(nlohmann::json domain, std::span<const TemplateDefinition> definitions, const nlohmann::json & asset);
    GraphTiming timing(const CoarseGraph & graph, std::span<const TemplateInterval> origins, nlohmann::json * first_gap = nullptr) const;
    std::int64_t external(const nlohmann::json & identity, WorkKind kind, const WorkFeatures & features) const;
    static nlohmann::json evaluate(const nlohmann::json & groups, const nlohmann::json & asset);

private:
    struct Bound {
        double lower;
        double upper;
    };

    struct Point {
        double coordinate;
        std::int64_t ns;
    };

    struct Curve {
        explicit Curve(const nlohmann::json & model);
        template<class Features>
        ServiceDuration estimate(const Features & features) const;

        std::map<std::string, Bound, std::less<>> bounds;
        std::optional<std::string> axis;
        std::vector<Point> points;
    };

    struct Estimate {
        const Curve * model;
        ServiceDuration service_ns;

        std::string_view status() const;
    };

    using PhaseCurves = std::map<std::string, Curve, std::less<>>;

    template<class Definitions>
    WorkCosts(nlohmann::json domain, const nlohmann::json & asset, const Definitions & definitions);
    std::optional<std::size_t> external_index(const nlohmann::json & identity, WorkKind kind) const;

    template<class Features>
    Estimate estimate(std::optional<std::size_t> definition, std::size_t interval, const Features & features) const;
    CostGap gap(const Estimate & estimate, const WorkFeatures & features, nlohmann::json details) const;

    nlohmann::json domain_;
    std::span<const TemplateDefinition> definitions_;
    std::vector<std::optional<std::size_t>> model_indices_;
    std::map<nlohmann::json, std::pair<std::size_t, WorkKind>> external_;
    std::vector<std::vector<PhaseCurves>> models_;
};

} // namespace markov
