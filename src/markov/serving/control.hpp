#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>

#include "markov/contracts/serving.hpp"
#include "markov/graph/types.hpp"

namespace markov {

// Control owns request progress and coordinates shared resources; the serving loop delivers execution feedback.
class ServingControl {
public:
    virtual ~ServingControl() = default;

    virtual std::optional<ServingAction> plan(std::int64_t now_ns) = 0;

    // Reads expose a result at operation return, then resume their caller at its declared continuation.
    virtual void finish(const ServingAction & action, std::size_t returned_event, std::size_t caller_returned_event, std::int64_t completed_ns,
                        const std::map<std::string, StorageBinding> & values) = 0;

    // An observed device boundary can wake polling even when no CPU action returns.
    virtual bool notify(std::size_t event) = 0;
    virtual std::set<std::size_t> retained_events() const = 0;

    virtual std::optional<std::int64_t> next_wakeup_ns(std::int64_t now_ns) const = 0;
    virtual bool done() const = 0;

    // Results read the owner's state without keeping another request or page ledger.
    virtual std::size_t free_pages() const = 0;
    virtual std::size_t request_count() const = 0;
    virtual const Request & request(std::size_t identity) const = 0;
};

} // namespace markov
