#include "markov/trace_graph/planning/token_paths.hpp"

#include <algorithm>

namespace markov::trace_graph::planning {

PrefixId TokenPaths::intern(std::span<const std::int64_t> tokens) {
    PrefixId path = 0;
    for (const auto token : tokens) {
        const auto [entry, inserted] = children_.try_emplace({ path, token }, nodes_.size());
        if (inserted) nodes_.push_back({ path, nodes_[path].depth + 1 });
        path = entry->second;
    }
    return path;
}

PrefixId TokenPaths::prefix(PrefixId path, std::size_t tokens) const {
    while (nodes_[path].depth > tokens) path = nodes_[path].parent;
    return path;
}

std::vector<PrefixId> TokenPaths::pages(PrefixId path, std::size_t page_size_tokens) const {
    std::vector<PrefixId> result;
    result.reserve(nodes_[path].depth / page_size_tokens);
    while (path != 0) {
        if (nodes_[path].depth % page_size_tokens == 0) result.push_back(path);
        path = nodes_[path].parent;
    }
    std::ranges::reverse(result);
    return result;
}

} // namespace markov::trace_graph::planning
