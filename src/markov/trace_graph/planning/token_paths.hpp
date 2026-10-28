#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace markov::trace_graph::planning {

using PrefixId = std::size_t;

// A logical prefix is identified by its complete token ancestry. This index is
// shared across targets; residency and page geometry are not source facts.
class TokenPaths {
public:
    PrefixId intern(std::span<const std::int64_t> tokens);
    std::size_t length(PrefixId path) const { return nodes_[path].depth; }
    PrefixId prefix(PrefixId path, std::size_t tokens) const;
    std::vector<std::int64_t> suffix(PrefixId path, std::size_t begin_tokens) const;
    std::optional<PrefixId> append_known(PrefixId prefix, std::span<const std::int64_t> tokens) const;
    // Intake supplies a positive page size; incomplete pages remain uncached.
    std::vector<PrefixId> pages(PrefixId path, std::size_t page_size_tokens) const;
    std::size_t size() const { return nodes_.size(); }

private:
    struct Node {
        PrefixId parent;
        std::size_t depth;
        std::int64_t token;
    };
    std::vector<Node> nodes_{
        { 0, 0, 0 }
    };
    // Edge labels retain actual tokens, not native dictionary hashes.
    std::map<std::pair<PrefixId, std::int64_t>, PrefixId> children_;
};

} // namespace markov::trace_graph::planning
