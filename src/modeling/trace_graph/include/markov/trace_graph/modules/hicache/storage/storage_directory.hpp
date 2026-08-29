/**
 * @file
 * @brief HiCache storage-visible page and backend-hash readability directory.
 */
#pragma once

#include "markov/trace_graph/modules/hicache/radix/token_radix_tree.hpp"
#include "markov/trace_graph/modules/hicache/runtime/target_pager.hpp"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace markov::trace_graph::modules::hicache::storage {

using radix::HiCacheNodeId;
using runtime::HiCachePagePath;
using runtime::HiCacheProjectedPage;

/**
 * @brief Storage-directory projection for one target page.
 *
 * Backend readability and radix-tree materialization are distinct dimensions. Release
 * policy needs only the normalized scope/hash key and readability; source attribution,
 * epochs, and materialization ownership are Debug diagnostics.
 */
struct HiCacheStorageRecord {
    std::string cache_scope;
    std::string page_hash;
    std::string storage_key;
    bool readable = false;
};

/**
 * @brief Readability record in the storage backend hash namespace.
 *
 * A backend record can exist before any radix materialization. SGLang storage-hit queries
 * target hash/prefix keys rather than requiring an existing host-radix node.
 */
struct HiCacheStorageBackendRecord {
    std::string storage_key;
    std::string cache_scope;
    std::string page_hash;
    bool readable = false;
};

/**
 * @brief Readability directory for the target storage namespace.
 *
 * Storage-hit policy first queries backend hashes and then inserts the contiguous hit
 * prefix into the host radix. This directory preserves that boundary: a readable storage
 * key does not require an existing host/device node.
 */
class HiCacheStorageDirectory {
public:
    /** @brief Observes a target page identity without making it storage-readable. */
    void observe_page(const HiCacheProjectedPage & page);

    /** @brief Observes every page in a target-projected path. */
    void observe_path(const HiCachePagePath & path);

    /** @brief Marks modeled storage-commit pages as readable. */
    void mark_readable_pages(const std::string & cache_scope, const std::vector<std::string> & page_ids);


    /** @brief Returns whether a page ID resolves to a readable storage key. */
    [[nodiscard]] bool readable(const std::string & page_id) const;

    /** @brief Returns whether a projected page's backend hash is readable. */
    [[nodiscard]] bool readable(const HiCacheProjectedPage & page) const;

    /** @brief Returns the contiguous readable prefix of page IDs. */
    [[nodiscard]] std::vector<std::string> contiguous_readable_prefix(const std::vector<std::string> & page_ids) const;

    /** @brief Returns the contiguous readable prefix of projected pages. */
    [[nodiscard]] std::vector<std::string> contiguous_readable_prefix(const std::vector<HiCacheProjectedPage> & pages) const;


private:
    std::unordered_map<std::string, HiCacheStorageRecord> records_by_page_;
    std::unordered_map<std::string, HiCacheStorageBackendRecord> records_by_storage_key_;

    [[nodiscard]] HiCacheStorageRecord & ensure_record(const std::string & cache_scope, const std::string & page_id);
    [[nodiscard]] HiCacheStorageRecord & ensure_record(const HiCacheProjectedPage & page);
    [[nodiscard]] HiCacheStorageBackendRecord & ensure_backend_record(const std::string & cache_scope, const std::string & page_hash);
    [[nodiscard]] static std::string storage_key(const std::string & cache_scope, const std::string & page_hash);
    void mark_record_readable(HiCacheStorageRecord & record, std::string_view source);
    void mark_backend_readable(HiCacheStorageBackendRecord & record, std::string_view source);
};

} // namespace markov::trace_graph::modules::hicache::storage
