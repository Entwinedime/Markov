#include "markov/contracts/input_group.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace markov {

InputGroup InputGroup::read(const nlohmann::json & source) {
    InputGroup group;
    group.dtype = source.at("dtype").get<std::string>();
    group.device = source.at("device").get<std::string>();
    group.member_dtypes = source.value("member_dtypes", std::vector<std::string>{});
    group.count = source.at("count").get<std::string>();
    for (const auto & [name, expression] : source.at("sums").items()) group.sums.emplace(name, Expression::read(expression));

    for (const auto & dimension : source.at("shape")) group.shape.push_back(Expression::read(dimension));

    return group;
}

const std::string & InputGroup::member_dtype(std::size_t index) const { return member_dtypes.empty() ? dtype : member_dtypes.at(index); }

void InputGroup::derive(Parameters & parameters, std::span<const Parameters> members) const {
    if (!member_dtypes.empty() && member_dtypes.size() != members.size()) throw std::invalid_argument("typed input group member count differs");

    Parameters derived;
    if (members.size() > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))
        throw std::overflow_error("input group exceeds the parameter range");

    derived.emplace(count, static_cast<std::int64_t>(members.size()));

    // Build collection aggregates independently of the caller's existing parameter bindings.
    for (const auto & [name, expression] : sums) {
        std::int64_t total = 0;

        for (const auto & member : members) {
            const auto value = expression.evaluate(member);
            if (value < 0 || total > std::numeric_limits<std::int64_t>::max() - value) throw std::invalid_argument("invalid input group aggregate: " + name);

            total += value;
        }

        if (!derived.emplace(name, total).second) throw std::invalid_argument("duplicate input group aggregate: " + name);
    }

    for (const auto & [name, value] : derived) {
        const auto [position, added] = parameters.emplace(name, value);
        if (!added && position->second != value) throw std::invalid_argument("input group disagrees with parameter " + name);
    }
}

std::vector<std::int64_t> InputGroup::dimensions(Parameters parameters, const Parameters & member) const {
    for (const auto & [name, value] : member) parameters.insert_or_assign(name, value);

    std::vector<std::int64_t> result;
    for (const auto & dimension : shape) {
        const auto value = dimension.evaluate(parameters);
        if (value < 0) throw std::invalid_argument("negative input group dimension");

        result.push_back(value);
    }

    return result;
}

std::optional<Parameters> InputGroup::infer_member(std::span<const std::int64_t> geometry, const Parameters & parameters) const {
    if (geometry.size() != shape.size()) return std::nullopt;

    Parameters member;
    std::set<std::string> member_names;
    for (const auto & [_, expression] : sums) member_names.merge(expression.variables());
    for (std::size_t axis = 0; axis < shape.size(); ++axis) {
        const auto dimension = shape[axis].document();
        if (!dimension.is_string()) continue;

        const auto name = dimension.get<std::string>();
        if (!member_names.contains(name)) continue;
        const auto [position, added] = member.emplace(name, geometry[axis]);
        if (!added && position->second != geometry[axis]) return std::nullopt;
    }

    // Only named axes can be inverted; aggregate expressions consume those members without fitting equations.
    for (const auto & name : member_names) {
        if (!member.contains(name)) return std::nullopt;
    }
    for (const auto & dimension : shape) {
        for (const auto & name : dimension.variables()) {
            if (!parameters.contains(name) && !member.contains(name)) return std::nullopt;
        }
    }

    if (!std::ranges::equal(dimensions(parameters, member), geometry)) return std::nullopt;

    return member;
}

} // namespace markov
