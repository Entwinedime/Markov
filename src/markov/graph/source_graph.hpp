#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "markov/contracts/expression.hpp"
#include "markov/graph/access.hpp"

namespace markov {

// Validate recorded geometry and value ownership before public interface projection.
class SourceGraph {
public:
    explicit SourceGraph(nlohmann::json source);

    nlohmann::json summary() const;
    const nlohmann::json & observations() const;
    const std::vector<ValueAccess> & value_accesses() const;

private:
    struct ShapeReport;

    struct Geometry {
        std::vector<std::int64_t> shape;
        std::optional<std::vector<Expression>> shape_relation;
    };

    Parameters parameters(std::size_t region) const;
    void inspect_guards(const nlohmann::json & metadata, const Parameters & parameters, const nlohmann::json & location, ShapeReport & report) const;
    void inspect_shape(std::size_t value, const Parameters & parameters, const nlohmann::json & location, ShapeReport & report) const;

    nlohmann::json source_;
    std::vector<Geometry> values_;
    std::vector<ValueAccess> value_accesses_;
    std::vector<Operation> operations_;
    std::size_t dependency_count_;
};

} // namespace markov
