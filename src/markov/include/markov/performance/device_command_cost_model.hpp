#pragma once

#include "markov/contracts/service_work.hpp"
#include "markov/performance/cost_model.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <map>
#include <optional>

namespace markov {

class DeviceCommandCostModel final : public CostModel {
public:
    explicit DeviceCommandCostModel(const nlohmann::json & entries);

    std::optional<std::int64_t> measured_cost(const DeviceCommandWork & work) const;
    CostModelResult estimate(const CostQuery & query) const override;
    NodeCostBasis basis() const override { return NodeCostBasis::DeviceCommandCandidate; }

private:
    std::map<DeviceCommandWork, std::int64_t> entries_;
};

} // namespace markov
