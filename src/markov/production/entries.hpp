#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "markov/graph/graph.hpp"

namespace markov {

// Detach one semantic call, exporting cross-cut dependencies instead of copying its neighbours.
class EntryExtraction {
public:
    explicit EntryExtraction(const CoarseGraph & source);
    CoarseGraph extract(std::size_t scope) const;

private:
    using SlotContent = std::tuple<std::string, std::string, std::string>;
    class Cut;
    struct ScopeContents {
        std::vector<std::size_t> children;
        std::vector<std::size_t> events;
        std::vector<std::size_t> work;
    };
    const CoarseGraph & source_;
    std::vector<ScopeContents> scopes_;
    std::vector<std::vector<std::size_t>> incoming_;
    std::vector<std::size_t> event_order_;
    std::map<std::size_t, std::set<std::pair<std::string, std::int64_t>>> outputs_;
    std::map<std::size_t, std::set<std::string>> signal_outputs_;
    std::map<std::size_t, std::set<SlotContent>> slot_outputs_;
};

} // namespace markov
