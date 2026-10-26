#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "markov/contracts/expression.hpp"

namespace markov {

using ParameterGroups = std::map<std::string, std::vector<Parameters>>;
using GroupValues = std::map<std::string, std::vector<std::size_t>>;

// Members share an element geometry; count and sums describe the whole collection.
struct InputGroup {
    std::string dtype;
    std::string device;
    std::vector<Expression> shape;
    std::vector<std::string> member_dtypes;

    std::string count;
    std::map<std::string, Expression> sums;

    static InputGroup read(const nlohmann::json & source);
    const std::string & member_dtype(std::size_t index) const;
    void derive(Parameters & parameters, std::span<const Parameters> members) const;
    std::vector<std::int64_t> dimensions(Parameters parameters, const Parameters & member) const;
    std::optional<Parameters> infer_member(std::span<const std::int64_t> geometry, const Parameters & parameters) const;
};

} // namespace markov
