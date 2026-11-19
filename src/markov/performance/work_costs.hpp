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
class TemplateLibrary;
class WorkFeatures;
enum class WorkKind;
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
    WorkCosts(const TemplateLibrary & library, const nlohmann::json & asset);
    GraphTiming timing(const CoarseGraph & graph, std::span<const TemplateInterval> origins) const;
    std::int64_t external(const nlohmann::json & identity, WorkKind kind, const WorkFeatures & features) const;

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
        ServiceDuration estimate(const WorkFeatures & features) const;

        std::map<std::string, Bound, std::less<>> bounds;
        std::optional<std::string> axis;
        std::vector<Point> points;
    };

    using PhaseCurves = std::map<std::string, Curve, std::less<>>;
    const Curve * curve(std::optional<std::size_t> definition, std::size_t interval, std::string_view phase) const;
    CostGap gap(const Curve * candidate, const WorkFeatures & features, nlohmann::json details) const;

    const TemplateLibrary & library_;
    std::vector<std::optional<std::size_t>> definitions_;
    std::map<nlohmann::json, std::pair<std::size_t, WorkKind>> external_;
    std::vector<std::vector<PhaseCurves>> models_;
};

} // namespace markov
