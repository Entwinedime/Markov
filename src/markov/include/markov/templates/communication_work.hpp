#pragma once

#include "markov/contracts/control.hpp"

#include <compare>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace markov {

enum class CommunicationOperation { AllReduce, AllGather, Broadcast };

enum class CollectiveRows { Tokens, Requests };

struct CollectiveCountRule {
    CollectiveRows rows;
    std::uint64_t width;
    std::uint64_t count(const BatchInput & input) const;
    auto operator<=>(const CollectiveCountRule &) const = default;
};

// The enclosing interface owns the framing position. This rule only binds
// sender and protocol completion constraints, without a wire-byte formula.
struct HostMessageRule {
    std::uint32_t root;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> completion_entry_dependencies;

    static std::expected<HostMessageRule, std::string_view> bind(std::uint32_t root, std::span<const std::uint32_t> ranks, const nlohmann::json & dependencies);
    auto operator<=>(const HostMessageRule &) const = default;
};

} // namespace markov
