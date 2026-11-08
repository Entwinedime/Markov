#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include <nlohmann/json.hpp>

#include "markov/resources/changes.hpp"

namespace markov {

struct KvPageRange;

// Observe the authoritative resource service; recording must not create a second allocation ledger.
class ResourceObserver {
public:
    virtual ~ResourceObserver() = default;
    virtual void applied(std::string_view group, const nlohmann::json & operation, const nlohmann::json & result, const ResourceChanges & changes,
                         std::int64_t time_ns) = 0;
    virtual void bound(std::size_t invocation, std::string_view pool, std::string_view request, std::optional<std::size_t> slot, std::int64_t begin,
                       std::span<const KvPageRange> ranges) = 0;
};

} // namespace markov
