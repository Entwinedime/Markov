#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "markov/execution/timing.hpp"

namespace markov {

class CoarseGraph;
class TemplateLibrary;
class WorkFeatures;
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
    const Curve * curve(const TemplateInterval & origin, std::string_view phase) const;

    std::vector<std::optional<std::size_t>> definitions_;
    std::vector<std::vector<PhaseCurves>> models_;
};

} // namespace markov
