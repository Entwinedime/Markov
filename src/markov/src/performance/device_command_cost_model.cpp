#include "markov/performance/device_command_cost_model.hpp"

#include "markov/contracts/json_input.hpp"

#include <nlohmann/json.hpp>

#include <limits>
#include <stdexcept>

namespace markov {

DeviceCommandCostModel::DeviceCommandCostModel(const nlohmann::json & entries) {
    for (const auto & entry : entries) {
        const auto work = DeviceCommandWork::read(entry.at("features"));
        const auto duration = input::integer(entry.at("estimate_ns"), 0, std::numeric_limits<std::int64_t>::max());
        if (!entries_.emplace(work, duration).second) throw std::invalid_argument("duplicate device command cost features");
    }
}

std::optional<std::int64_t> DeviceCommandCostModel::measured_cost(const DeviceCommandWork & work) const {
    const auto found = entries_.find(work);
    if (found == entries_.end()) return std::nullopt;
    return found->second;
}

CostModelResult DeviceCommandCostModel::estimate(const CostQuery & query) const {
    for (const auto & object : query.objects)
        if (object.kind == CostQueryKind::DeviceCommand && object.issue == CostQueryIssue::None)
            return measured(measured_cost(std::get<DeviceCommandWork>(object.features)));
    return std::unexpected(CostModelGap{});
}

} // namespace markov
