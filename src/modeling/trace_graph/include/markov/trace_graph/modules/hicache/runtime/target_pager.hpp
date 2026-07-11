/**
 * @file
 * @brief Projects HiCache token paths into target page identities.
 */
#pragma once

#include "markov/trace_graph/frontend/model_config.hpp"
#include "markov/trace_graph/modules/hicache/fact.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace markov::trace_graph::modules::hicache::runtime {

/**
 * @brief Traceable projection of one complete target page.
 *
 * `id` is the stable state-model identity. The remaining fields preserve the scope and token
 * interval from which that identity was derived.
 */
struct HiCacheProjectedPage {
    std::string id;
    std::string cache_scope;
    std::string hash;
    uint64_t page_index = 0;
    uint64_t token_begin = 0;
    uint64_t token_end = 0;
};

/**
 * @brief Complete-page projection of one request path under the target page size.
 */
struct HiCachePagePath {
    std::string cache_scope;
    uint64_t page_size = 0;
    std::vector<HiCacheProjectedPage> pages;

    /** @brief Returns whether the path contains no complete target page. */
    [[nodiscard]] bool empty() const { return pages.empty(); }

    /** @brief Returns the number of complete projected pages. */
    [[nodiscard]] size_t size() const { return pages.size(); }

    /** @brief Copies stable page IDs in request-path order. */
    [[nodiscard]] std::vector<std::string> page_ids() const;
};

/**
 * @brief Pure target-page projector for token paths.
 *
 * This is the isolation boundary between source page identity and target state. IDs are
 * derived from tokens, chained hashes, cache scope, and target page size without consulting
 * source residency.
 */
class HiCacheTargetPager {
public:
    explicit HiCacheTargetPager(frontend::HiCacheConfig config = frontend::HiCacheConfig{});

    /** @brief Resolves target page size, using the fact's source size only when config omits it. */
    [[nodiscard]] uint64_t page_size_for_fact(const HiCacheFact & fact) const;

    /** @brief Builds a scope-qualified internal page ID. */
    [[nodiscard]] std::string scoped_page_id(const std::string & cache_scope, const std::string & page_hash) const;

    /** @brief Projects a token path and drops its incomplete trailing page. */
    [[nodiscard]] HiCachePagePath project(const HiCacheFact & fact, const HiCacheTokenPath & tokens) const;

private:
    frontend::HiCacheConfig config_;
};

} // namespace markov::trace_graph::modules::hicache::runtime
