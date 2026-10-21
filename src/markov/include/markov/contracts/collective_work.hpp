#pragma once

#include <nlohmann/json_fwd.hpp>

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace markov {

// The logical request cohort is known before native encoding. Its lengths do
// not determine pickle metadata or wire bytes; an absent extent is deliberate.
struct HostMessageWork {
    std::uint32_t root;
    std::optional<std::uint64_t> wire_bytes;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> incoming_requests;

    static HostMessageWork read(const nlohmann::json & value);
    nlohmann::json describe() const;
    auto operator<=>(const HostMessageWork &) const = default;
};

// Logical work is shared by local CPU service and rank device service. Native
// queue identities and timing do not change the operation being performed.
struct CollectiveWork {
    std::string operation;
    std::vector<std::uint32_t> ranks;
    std::vector<std::uint64_t> input_shape;
    std::string data_type;
    std::optional<HostMessageWork> host_message = std::nullopt;

    static CollectiveWork read(const nlohmann::json & value);
    nlohmann::json describe() const;
    auto operator<=>(const CollectiveWork &) const = default;
};

} // namespace markov
