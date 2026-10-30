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

namespace markov {

class ResourceTrace;

// One visited compressed edge; a split preserves the old suffix handle and creates this prefix handle.
struct PrefixStep {
    std::size_t node;
    std::int64_t matched_tokens;
    std::optional<std::size_t> split_prefix;
};

struct PrefixMatch {
    std::size_t node = 0;
    std::size_t matched_tokens = 0;
    std::vector<PrefixStep> path;
};

struct PrefixInsertion {
    std::size_t node;
    std::int64_t existing_tokens;
    std::vector<PrefixStep> path;
    bool inserted;
};

struct PrefixResidence {
    std::size_t node;
    std::int64_t references;
    std::span<const std::size_t> pages;
};

// Content owns page bindings; the pool still owns allocation and execution owns content readiness.
class PrefixIndex {
public:
    explicit PrefixIndex(std::int64_t page_size, ResourceTrace * trace = nullptr, std::string pool = {});

    PrefixMatch match(std::span<const std::int64_t> tokens);
    std::vector<std::size_t> binding(std::size_t node) const;
    PrefixInsertion insert(std::span<const std::int64_t> tokens, std::span<const std::size_t> pages);
    std::int64_t lock(std::size_t node, bool acquire);
    std::vector<std::size_t> evict(std::int64_t tokens);
    std::int64_t evictable_tokens() const;
    std::int64_t protected_tokens() const;

    // Borrowed bindings remain valid until the next index mutation; no content or tree copy is needed.
    std::vector<PrefixResidence> residences() const;

private:
    using PageKey = std::vector<std::int64_t>;
    struct Node {
        std::size_t parent = 0;
        std::vector<std::int64_t> tokens;
        std::vector<std::size_t> pages;
        std::map<PageKey, std::size_t> children;
        std::int64_t references = 0;
        std::size_t recency = 0;
    };

    PrefixMatch walk(std::span<const std::int64_t> tokens);
    std::size_t split(std::size_t node, std::size_t tokens);
    PageKey first_page(std::span<const std::int64_t> tokens) const;
    void touch(std::size_t node, std::size_t recency);
    void update_leaf(std::size_t node);

    std::size_t page_size_;
    std::size_t next_node_ = 1;
    std::size_t clock_ = 0;
    std::map<std::size_t, Node> nodes_;
    std::set<std::pair<std::size_t, std::size_t>> leaves_;
    std::int64_t evictable_tokens_ = 0;
    std::int64_t protected_tokens_ = 0;
    ResourceTrace * trace_;
    std::string pool_;
};

} // namespace markov
