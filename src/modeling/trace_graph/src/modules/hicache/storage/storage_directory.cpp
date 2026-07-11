/**
 * @file
 * @brief HiCache target storage-readability directory implementation.
 */
#include "markov/trace_graph/modules/hicache/storage/storage_directory.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace markov::trace_graph::modules::hicache::storage {

namespace storage_directory_detail {

std::string normalized_scope(std::string cache_scope) { return cache_scope.empty() ? std::string("-1") : std::move(cache_scope); }

std::string page_hash_from_id(const std::string & page_id) {
    const auto delimiter = page_id.find('|');
    if (delimiter == std::string::npos) return page_id;
    return page_id.substr(delimiter + 1);
}

std::string page_scope_from_id(const std::string & page_id) {
    const auto delimiter = page_id.find('|');
    if (delimiter == std::string::npos) return "-1";
    return page_id.substr(0, delimiter);
}

} // namespace storage_directory_detail

using storage_directory_detail::normalized_scope;
using storage_directory_detail::page_hash_from_id;
using storage_directory_detail::page_scope_from_id;

std::string HiCacheStorageDirectory::storage_key(const std::string & cache_scope, const std::string & page_hash) {
    return normalized_scope(cache_scope) + "|" + page_hash;
}

/**
 * @brief Returns or creates a backend hash-namespace record.
 *
 * A backend record means that one scope/hash can become L3-readable independently of
 * radix materialization. Prefetch may discover the hash first and insert the readable
 * prefix into the host tree at a later target cache-extend boundary.
 */
HiCacheStorageBackendRecord & HiCacheStorageDirectory::ensure_backend_record(const std::string & cache_scope, const std::string & page_hash) {
    const auto key = storage_key(cache_scope, page_hash);
    auto [it, inserted] = records_by_storage_key_.try_emplace(key);
    auto & record = it->second;
    if (inserted) {
        record.storage_key = key;
        record.cache_scope = normalized_scope(cache_scope);
        record.page_hash = page_hash;
#ifdef DEBUG
        record.known_epoch = core::checked_increment_u64(epoch_, "HiCache storage directory epoch exceeds uint64 range");
#endif
    }
    return record;
}

HiCacheStorageRecord & HiCacheStorageDirectory::ensure_record(const std::string & cache_scope, const std::string & page_id) {
    // A page record projects one target page ID. If its backend hash is already readable,
    // the new page inherits that state instead of waiting for another observation.
    if (page_id.empty()) throw std::invalid_argument("HiCache storage page ID must not be empty");
    auto [it, inserted] = records_by_page_.try_emplace(page_id);
    auto & record = it->second;
    if (inserted) {
#ifdef DEBUG
        record.page_id = page_id;
#endif
        record.cache_scope = cache_scope.empty() ? page_scope_from_id(page_id) : normalized_scope(cache_scope);
        record.page_hash = page_hash_from_id(page_id);
        record.storage_key = storage_key(record.cache_scope, record.page_hash);
#ifdef DEBUG
        record.known_epoch = core::checked_increment_u64(epoch_, "HiCache storage directory epoch exceeds uint64 range");
#endif
    }
    else if (!cache_scope.empty() && record.cache_scope != normalized_scope(cache_scope)) {
        throw std::logic_error("HiCache storage page ID was observed with conflicting cache scopes: " + page_id);
    }
    auto & backend = ensure_backend_record(record.cache_scope, record.page_hash);
    if (backend.readable && !record.readable) {
        record.readable = true;
#ifdef DEBUG
        record.readable_source = backend.readable_source;
        record.readable_epoch = backend.readable_epoch;
#endif
    }
    return record;
}

HiCacheStorageRecord & HiCacheStorageDirectory::ensure_record(const HiCacheProjectedPage & page) {
    if (page.hash.empty()) throw std::invalid_argument("HiCache projected page hash must not be empty");
    const auto expected_id = storage_key(page.cache_scope, page.hash);
    if (page.id != expected_id) throw std::invalid_argument("HiCache projected page ID does not match its scope and hash: " + page.id);
    auto & record = ensure_record(page.cache_scope, page.id);
    if (record.page_hash != page.hash || record.storage_key != expected_id)
        throw std::logic_error("HiCache storage page identity changed after insertion: " + page.id);
    return record;
}

void HiCacheStorageDirectory::mark_backend_readable(HiCacheStorageBackendRecord & record, std::string_view source) {
#ifdef DEBUG
    if (!record.readable) record.readable_epoch = core::checked_increment_u64(epoch_, "HiCache storage directory epoch exceeds uint64 range");
#endif
    record.readable = true;
#ifdef DEBUG
    if (!source.empty()) record.readable_source = std::string(source);
#else
    (void)source;
#endif
}

void HiCacheStorageDirectory::mark_record_readable(HiCacheStorageRecord & record, std::string_view source) {
#ifdef DEBUG
    if (!record.readable) record.readable_epoch = core::checked_increment_u64(epoch_, "HiCache storage directory epoch exceeds uint64 range");
#endif
    record.readable = true;
#ifdef DEBUG
    if (!source.empty()) record.readable_source = std::string(source);
#endif
    auto & backend = ensure_backend_record(record.cache_scope, record.page_hash);
#ifdef DEBUG
    mark_backend_readable(backend, record.readable_source);
#else
    mark_backend_readable(backend, source);
#endif
}

void HiCacheStorageDirectory::observe_page(const HiCacheProjectedPage & page) {
    auto & record = ensure_record(page);
#ifdef DEBUG
    record.known = true;
#else
    (void)record;
#endif
}

void HiCacheStorageDirectory::observe_path(const HiCachePagePath & path) {
    std::ranges::for_each(path.pages, [&](const auto & page) { observe_page(page); });
}

void HiCacheStorageDirectory::mark_readable_pages(const std::string & cache_scope, const std::vector<std::string> & page_ids) {
    std::ranges::for_each(page_ids, [&](const auto & page_id) {
        auto & record = ensure_record(cache_scope, page_id);
#ifdef DEBUG
        record.known = true;
#endif
        mark_record_readable(record, "modeled_storage_commit");
    });
}

#ifdef DEBUG
void HiCacheStorageDirectory::mark_materialized_pages(const std::vector<std::string> & page_ids, HiCacheNodeId node_id) {
    // Materialization records page-to-radix ownership for Debug derivation. It does not
    // alter backend readability.
    std::ranges::for_each(page_ids, [&](const auto & page_id) {
        auto & record = ensure_record("", page_id);
        record.known = true;
        if (!record.materialized_node) record.materialized_epoch = core::checked_increment_u64(epoch_, "HiCache storage directory epoch exceeds uint64 range");
        record.materialized_node = node_id;
        auto & backend = ensure_backend_record(record.cache_scope, record.page_hash);
        backend.materialized_pages.insert(page_id);
    });
}
#endif

bool HiCacheStorageDirectory::readable(const std::string & page_id) const {
    const auto it = records_by_page_.find(page_id);
    if (it != records_by_page_.end()) {
        if (it->second.readable) return true;
        const auto backend_it = records_by_storage_key_.find(it->second.storage_key);
        return backend_it != records_by_storage_key_.end() && backend_it->second.readable;
    }
    const auto backend_it = records_by_storage_key_.find(page_id);
    return backend_it != records_by_storage_key_.end() && backend_it->second.readable;
}

bool HiCacheStorageDirectory::readable(const HiCacheProjectedPage & page) const {
    if (const auto it = records_by_page_.find(page.id); it != records_by_page_.end() && it->second.readable) return true;
    const auto backend_it = records_by_storage_key_.find(storage_key(page.cache_scope, page.hash.empty() ? page_hash_from_id(page.id) : page.hash));
    return backend_it != records_by_storage_key_.end() && backend_it->second.readable;
}

std::vector<std::string> HiCacheStorageDirectory::contiguous_readable_prefix(const std::vector<std::string> & page_ids) const {
    std::vector<std::string> prefix;
    prefix.reserve(page_ids.size());
    for (const auto & page_id : page_ids) {
        if (!readable(page_id)) break;
        prefix.push_back(page_id);
    }
    return prefix;
}

std::vector<std::string> HiCacheStorageDirectory::contiguous_readable_prefix(const std::vector<HiCacheProjectedPage> & pages) const {
    // SGLang accepts only a contiguous storage-hit prefix. Readable pages after the first
    // gap cannot contribute to this prefetch result.
    std::vector<std::string> prefix;
    prefix.reserve(pages.size());
    for (const auto & page : pages) {
        if (!readable(page)) break;
        prefix.push_back(page.id);
    }
    return prefix;
}

#ifdef DEBUG
std::set<std::string> HiCacheStorageDirectory::readable_page_ids(bool include_backend_only) const {
    std::set<std::string> pages;
    std::ranges::for_each(records_by_page_ | std::views::values, [&](const auto & record) {
        if (readable(record.page_id)) pages.insert(record.page_id);
    });
    if (include_backend_only) {
        std::ranges::for_each(records_by_storage_key_ | std::views::values, [&](const auto & record) {
            if (record.readable) pages.insert(record.storage_key);
        });
    }
    return pages;
}

uint64_t HiCacheStorageDirectory::known_page_count() const {
    return static_cast<uint64_t>(std::ranges::count_if(records_by_page_, [](const auto & item) { return item.second.known; }));
}

uint64_t HiCacheStorageDirectory::readable_page_count() const {
    return static_cast<uint64_t>(std::ranges::count_if(records_by_page_, [&](const auto & item) {
        if (item.second.readable) return true;
        const auto backend_it = records_by_storage_key_.find(item.second.storage_key);
        return backend_it != records_by_storage_key_.end() && backend_it->second.readable;
    }));
}

uint64_t HiCacheStorageDirectory::backend_readable_count() const {
    return static_cast<uint64_t>(std::ranges::count_if(records_by_storage_key_, [](const auto & item) { return item.second.readable; }));
}

uint64_t HiCacheStorageDirectory::materialized_page_count() const {
    return static_cast<uint64_t>(std::ranges::count_if(records_by_page_, [](const auto & item) { return item.second.materialized_node.has_value(); }));
}
#endif

} // namespace markov::trace_graph::modules::hicache::storage
