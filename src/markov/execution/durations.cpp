#include "markov/execution/durations.hpp"

#include <stdexcept>
#include <string>

namespace markov {

GivenDurations::GivenDurations(const nlohmann::json & values) {
    if (!values.is_object()) throw std::invalid_argument("given node durations must be an object of nanosecond values");
    for (const auto & [name, value] : values.items()) {
        if (!value.is_number_integer() || value.get<std::int64_t>() < 0)
            throw std::invalid_argument("given node duration must be a nonnegative integer: " + name);
        values_.emplace(execution_kind(name), value.get<std::int64_t>());
    }
}

std::int64_t GivenDurations::duration_ns(const RegionGraph &, const ExecutionNode & node) const {
    const auto value = values_.find(node.step.kind);
    if (value == values_.end()) throw std::invalid_argument("missing given duration for " + std::string(execution_name(node.step.kind)));
    return value->second;
}

} // namespace markov
