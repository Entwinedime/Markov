#pragma once

#include "markov/trace_graph/planning/token_paths.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace markov::trace_graph::planning {

using RadixId = std::size_t;

struct RadixNode {
    RadixId parent;
    std::vector<PrefixId> pages;
    std::map<PrefixId, RadixId> children;
};

struct RadixMatch {
    RadixId node;
    std::size_t pages;
    std::optional<RadixId> split_child = std::nullopt;
};

// Keys are complete page-end token prefixes from one shared TokenPaths index.
// Node IDs survive splits. Geometry alone does not imply residency or visibility.
class RadixTree {
public:
    // Matching may split an existing segment at the query's common prefix.
    RadixMatch match(std::span<const PrefixId> pages);
    // Insertion returns the leaf and the number of pages that already existed.
    // A split child lets the cache owner divide residency and held references.
    RadixMatch insert(std::span<const PrefixId> pages);
    void erase_leaf(RadixId node);
    std::span<const RadixNode> nodes() const { return nodes_; }

private:
    RadixId split(RadixId child, std::size_t prefix_pages);

    std::vector<RadixNode> nodes_{
        { 0, {}, {} }
    };
};

} // namespace markov::trace_graph::planning
