#include "markov/resources/prefix_index.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "markov/resources/trace.hpp"

namespace markov {

PrefixIndex::PrefixIndex(std::int64_t page_size, ResourceTrace * trace, std::string pool)
    : page_size_(static_cast<std::size_t>(page_size)),
      trace_(trace),
      pool_(std::move(pool)) {
    if (page_size <= 0) throw std::invalid_argument("prefix index requires a positive page size");
    nodes_.try_emplace(0).first->second.references = 1;
}

PrefixIndex::PageKey PrefixIndex::first_page(std::span<const std::int64_t> tokens) const {
    const auto page = tokens.first(page_size_);
    return { page.begin(), page.end() };
}

void PrefixIndex::update_leaf(std::size_t identity) {
    const auto & node = nodes_.at(identity);
    const auto entry = std::pair{ node.recency, identity };
    if (identity != 0 && node.references == 0 && node.children.empty()) leaves_.insert(entry);
    else leaves_.erase(entry);
}

void PrefixIndex::touch(std::size_t identity, std::size_t recency) {
    auto & node = nodes_.at(identity);
    leaves_.erase({ node.recency, identity });
    node.recency = recency;
    update_leaf(identity);
    if (trace_ && identity != 0)
        trace_->append(pool_,
                       "prefix_touch",
                       {
                           {    "node", identity },
                           { "recency",  recency }
        });
}

std::size_t PrefixIndex::split(std::size_t identity, std::size_t tokens) {
    auto & child = nodes_.at(identity);
    const auto pages = tokens / page_size_;
    const auto parent_key = first_page(child.tokens);
    const auto suffix_key = first_page(std::span(child.tokens).subspan(tokens));
    const auto prefix = next_node_++;
    auto & node = nodes_.try_emplace(prefix).first->second;
    node.parent = child.parent;
    node.tokens.assign(child.tokens.begin(), child.tokens.begin() + static_cast<std::ptrdiff_t>(tokens));
    node.pages.assign(child.pages.begin(), child.pages.begin() + static_cast<std::ptrdiff_t>(pages));
    node.children.emplace(suffix_key, identity);
    node.references = child.references;
    node.recency = ++clock_;
    nodes_.at(child.parent).children.at(parent_key) = prefix;

    // Existing handles still name the suffix; releasing one walks through its new shared parent.
    child.parent = prefix;
    child.tokens.erase(child.tokens.begin(), child.tokens.begin() + static_cast<std::ptrdiff_t>(tokens));
    child.pages.erase(child.pages.begin(), child.pages.begin() + static_cast<std::ptrdiff_t>(pages));
    if (trace_)
        trace_->append(pool_,
                       "prefix_split",
                       {
                           {       "node",        identity },
                           {     "prefix",          prefix },
                           {     "parent",     node.parent },
                           {      "pages",      node.pages },
                           { "references", node.references },
                           {    "recency",    node.recency }
        });
    return prefix;
}

PrefixMatch PrefixIndex::walk(std::span<const std::int64_t> tokens) {
    PrefixMatch result;
    const auto recency = ++clock_;
    touch(0, recency);
    while (!tokens.empty()) {
        const auto & children = nodes_.at(result.node).children;
        const auto found = children.find(first_page(tokens));
        if (found == children.end()) break;

        auto identity = found->second;
        touch(identity, recency);
        const auto & child = nodes_.at(identity);
        const auto end = std::ranges::mismatch(tokens, child.tokens).in1;
        const auto matched = static_cast<std::size_t>(end - tokens.begin()) / page_size_ * page_size_;
        PrefixStep step{ identity, static_cast<std::int64_t>(matched), std::nullopt };
        if (matched < child.tokens.size()) {
            identity = split(identity, matched);
            step.split_prefix = identity;
        }
        result.path.push_back(step);

        result.matched_tokens += matched;
        result.node = identity;
        tokens = tokens.subspan(matched);
    }
    return result;
}

PrefixMatch PrefixIndex::match(std::span<const std::int64_t> tokens) {
    tokens = tokens.first(tokens.size() / page_size_ * page_size_);
    return tokens.empty() ? PrefixMatch{} : walk(tokens);
}

std::vector<std::size_t> PrefixIndex::binding(std::size_t identity) const {
    std::vector<const Node *> path;
    std::size_t count = 0;
    while (identity != 0) {
        const auto & node = nodes_.at(identity);
        path.push_back(&node);
        count += node.pages.size();
        identity = node.parent;
    }

    // Reading an existing handle does not perform another lookup or change its eviction priority.
    std::vector<std::size_t> pages;
    pages.reserve(count);
    for (auto node = path.rbegin(); node != path.rend(); ++node) { pages.insert(pages.end(), (*node)->pages.begin(), (*node)->pages.end()); }
    return pages;
}

PrefixInsertion PrefixIndex::insert(std::span<const std::int64_t> tokens, std::span<const std::size_t> pages) {
    tokens = tokens.first(tokens.size() / page_size_ * page_size_);
    if (pages.size() != tokens.size() / page_size_) throw std::invalid_argument("cached content and page bindings have different lengths");
    auto matched = walk(tokens);
    const auto existing = matched.matched_tokens;
    const auto suffix = tokens.subspan(existing);
    if (!suffix.empty()) {
        const auto identity = next_node_++;
        auto & node = nodes_.try_emplace(identity).first->second;
        node.parent = matched.node;
        node.tokens.assign(suffix.begin(), suffix.end());
        const auto new_pages = pages.subspan(existing / page_size_);
        node.pages.assign(new_pages.begin(), new_pages.end());
        node.recency = ++clock_;
        nodes_.at(matched.node).children.emplace(first_page(suffix), identity);
        evictable_tokens_ += static_cast<std::int64_t>(suffix.size());
        update_leaf(matched.node);
        update_leaf(identity);
        if (trace_)
            trace_->append(pool_,
                           "prefix_insert",
                           {
                               {    "node",     identity },
                               {  "parent",  node.parent },
                               {   "pages",   node.pages },
                               { "recency", node.recency }
            });
        matched.node = identity;
    }
    return { matched.node, static_cast<std::int64_t>(existing), std::move(matched.path), !suffix.empty() };
}

std::int64_t PrefixIndex::lock(std::size_t identity, bool acquire) {
    std::int64_t delta = 0;
    nlohmann::json changed;
    if (trace_) changed = nlohmann::json::array();
    while (identity != 0) {
        auto & node = nodes_.at(identity);
        if (!acquire && node.references == 0) throw std::invalid_argument("cached prefix released without a reference");
        if (node.references == (acquire ? 0 : 1)) {
            const auto tokens = static_cast<std::int64_t>(node.tokens.size());
            delta += acquire ? -tokens : tokens;
        }
        node.references += acquire ? 1 : -1;
        if (trace_)
            changed.push_back({
                {       "node",        identity },
                { "references", node.references }
            });
        update_leaf(identity);
        identity = node.parent;
    }
    evictable_tokens_ += delta;
    protected_tokens_ -= delta;
    if (trace_ && !changed.empty())
        trace_->append(pool_,
                       "prefix_lock",
                       {
                           { "acquire",            acquire },
                           {   "nodes", std::move(changed) }
        });
    return delta;
}

std::vector<std::size_t> PrefixIndex::evict(std::int64_t tokens) {
    if (tokens < 0) throw std::invalid_argument("prefix eviction demand must be nonnegative");
    std::vector<std::size_t> released;
    while (tokens > 0 && !leaves_.empty()) {
        const auto identity = leaves_.begin()->second;
        auto & node = nodes_.at(identity);
        const auto parent = node.parent;
        const auto count = static_cast<std::int64_t>(node.tokens.size());
        if (trace_)
            trace_->append(pool_,
                           "prefix_evict",
                           {
                               { "node", identity }
            });
        nodes_.at(parent).children.erase(first_page(node.tokens));
        released.push_back(identity);
        leaves_.erase(leaves_.begin());
        nodes_.erase(identity);
        update_leaf(parent);
        evictable_tokens_ -= count;
        tokens -= count;
    }

    // Each compressed leaf is one release, including overshoot; callers commit pool reuse after graph completion.
    return released;
}

std::int64_t PrefixIndex::evictable_tokens() const { return evictable_tokens_; }
std::int64_t PrefixIndex::protected_tokens() const { return protected_tokens_; }

std::vector<PrefixResidence> PrefixIndex::residences() const {
    std::vector<PrefixResidence> result;
    result.reserve(nodes_.size() - 1);
    for (const auto & [identity, node] : nodes_) {
        if (identity != 0) result.push_back({ identity, node.references, node.pages });
    }
    return result;
}

} // namespace markov
