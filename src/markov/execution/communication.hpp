#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "markov/execution/timeline.hpp"
#include "markov/graph/types.hpp"

namespace markov {

// Ordered communication channels are independent of native control-message transport.
class CommunicationLinks {
public:
    void insert(const CoarseGraph & graph, std::span<const std::size_t> events, Timeline & timeline);
    bool empty() const;
    std::set<std::size_t> retained_events() const;
    nlohmann::json payload_coverage() const;

private:
    struct Endpoint {
        std::size_t ready;
        std::size_t completion;
        std::vector<std::int64_t> required;
        bool connected = false;
    };
    struct Exchange {
        std::string operation;
        std::vector<std::int64_t> members;
        std::map<std::tuple<Direction, std::string, bool>, TensorLayout> payload;
        std::map<std::int64_t, Endpoint> endpoints;
    };

    std::map<std::pair<std::string, std::int64_t>, std::size_t> positions_;
    std::map<std::pair<std::string, std::size_t>, Exchange> pending_;
    std::size_t payload_endpoints_ = 0;
    std::size_t unknown_endpoints_ = 0;
    std::size_t shape_fields_ = 0;
    std::size_t dtype_fields_ = 0;
};

} // namespace markov
