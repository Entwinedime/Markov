#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "markov/execution/timeline.hpp"
#include "markov/graph/types.hpp"

namespace markov {

// Ordered communication channels are independent of native control-message transport.
class CommunicationLinks {
public:
    void insert(const CoarseGraph & graph, std::span<const std::size_t> events, Timeline & timeline);
    bool empty() const;
    std::set<std::size_t> retained_events() const;

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
        std::optional<TensorLayout> payload;
        std::map<std::int64_t, Endpoint> endpoints;
    };

    std::map<std::pair<std::string, std::int64_t>, std::size_t> positions_;
    std::map<std::pair<std::string, std::size_t>, Exchange> pending_;
};

} // namespace markov
