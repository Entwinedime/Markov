#pragma once

#include "markov/performance/cost_query.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>

namespace markov {

enum class NodeCostBasis { Given, KernelCandidate, CommunicationCandidate, TransferCandidate, DeviceCommandCandidate, CpuCandidate };

// An empty reason means this candidate did not match. Contract gaps originate
// in the actual lookup, so coverage never reimplements model applicability.
struct CostModelGap {
    CostQueryIssue issue = CostQueryIssue::None;
    std::string_view reason = {};
};

using CostModelResult = std::expected<std::int64_t, CostModelGap>;

// Models consume target work through the same query. The workflow owns their
// deployment applicability and lifetime; queries never collect or refit.
class CostModel {
public:
    virtual ~CostModel() = default;
    virtual CostModelResult estimate(const CostQuery & query) const = 0;
    virtual NodeCostBasis basis() const = 0;

protected:
    static CostModelResult measured(std::optional<std::int64_t> duration) {
        if (duration) return *duration;
        return std::unexpected(CostModelGap{});
    }
};

} // namespace markov
