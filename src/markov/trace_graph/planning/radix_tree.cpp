#include "markov/trace_graph/planning/radix_tree.hpp"

#include <algorithm>
#include <utility>

namespace markov::trace_graph::planning {

RadixMatch RadixTree::match(std::span<const PrefixId> pages) {
    RadixId node = 0;
    std::size_t matched = 0;
    while (matched < pages.size()) {
        const auto found = nodes_[node].children.find(pages[matched]);
        if (found == nodes_[node].children.end()) break;

        const auto child = found->second;
        const auto & segment = nodes_[child].pages;
        const auto remaining = pages.subspan(matched);
        const auto common = static_cast<std::size_t>(std::ranges::mismatch(segment, remaining).in1 - segment.begin());

        matched += common;
        node = child;
        if (common < segment.size()) {
            node = split(child, common);
            return { node, matched, child };
        }
    }
    return { node, matched };
}

RadixMatch RadixTree::insert(std::span<const PrefixId> pages) {
    const auto matched = match(pages);
    if (matched.pages == pages.size()) return matched;

    const auto id = nodes_.size();
    nodes_.push_back({
        matched.node,
        { pages.begin() + matched.pages, pages.end() },
        {}
    });
    nodes_[matched.node].children.emplace(pages[matched.pages], id);
    return { id, matched.pages, matched.split_child };
}

void RadixTree::erase_leaf(RadixId node) { nodes_[nodes_[node].parent].children.erase(nodes_[node].pages.front()); }

RadixId RadixTree::split(RadixId child, std::size_t prefix_pages) {
    const auto parent = nodes_[child].parent;
    const auto key = nodes_[child].pages.front();
    const auto suffix_key = nodes_[child].pages[prefix_pages];
    std::vector<PrefixId> prefix(nodes_[child].pages.begin(), nodes_[child].pages.begin() + prefix_pages);

    const auto id = nodes_.size();
    nodes_.push_back({ parent, std::move(prefix), { { suffix_key, child } } });
    nodes_[parent].children[key] = id;
    nodes_[child].parent = id;
    nodes_[child].pages.erase(nodes_[child].pages.begin(), nodes_[child].pages.begin() + prefix_pages);
    return id;
}

} // namespace markov::trace_graph::planning
