#pragma once

#include "markov/trace_graph/graph/graph.hpp"
#include "markov/trace_graph/planning/work.hpp"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace markov::trace_graph::cost {

struct Range {
    double lower;
    double upper;
};

struct Affine {
    double intercept_ns;
    planning::Features coefficients_ns;
};

struct CoefficientPoint {
    double coordinate;
    Affine response;
};

struct CoefficientCurve {
    std::string feature;
    std::vector<CoefficientPoint> points;
};

using Formula = std::variant<Affine, CoefficientCurve>;

struct Model {
    planning::WorkKind kind;
    Formula response;
    std::map<std::string, Range> ranges;
    std::set<std::vector<double>> measured_points;
    std::set<std::string> extrapolation_features;
    bool allow_new_combinations;
    std::vector<std::string> sources;
};

enum class Status { Ready, MissingModel, MissingFeatures, OutsideDomain, UnmeasuredCombination, InvalidResponse };
enum class Coverage { Measured, NewCombination, Extrapolated, SourceService, SourceResidual };

struct Response {
    Status status;
    std::optional<graph::Time> duration_ns;
    std::optional<Coverage> coverage;
    std::vector<std::string> affected_features;
    std::optional<std::size_t> model;
};

// Immutable, indexed once per prediction session. No database, fitting, target
// identities or structural decisions are reachable through this interface.
class Models {
public:
    explicit Models(std::vector<Model> models);
    Response query(const planning::Work & work) const;
    const Model & model(std::size_t index) const { return models_[index]; }
    std::size_t size() const { return models_.size(); }

private:
    std::vector<Model> models_;
    std::map<planning::WorkKind, std::size_t> index_;
};

} // namespace markov::trace_graph::cost
