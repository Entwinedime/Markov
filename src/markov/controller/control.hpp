#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "markov/contracts/serving.hpp"

namespace markov {

// Control owns request progress and coordinates shared resources; workflow delivers execution feedback.
class ServingControl {
public:
    virtual ~ServingControl() = default;

    virtual std::optional<ServingAction> plan(std::int64_t now_ns, const GroupAdmission & admission) = 0;

    // Preparing native work may reserve resources; start accepts execution and finish commits completion effects.
    virtual void start(const ServingAction & action) = 0;
    virtual void finish(const ServingAction & action, const BatchProgram & program, std::int64_t completed_ns) = 0;

    virtual std::optional<std::int64_t> next_arrival_ns(std::int64_t now_ns) const = 0;
    virtual bool done() const = 0;

    // Results read the owner's state without keeping another request or page ledger.
    virtual std::size_t free_pages() const = 0;
    virtual std::size_t request_count() const = 0;
    virtual const Request & request(std::size_t identity) const = 0;
};

} // namespace markov
