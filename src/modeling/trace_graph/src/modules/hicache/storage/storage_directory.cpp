/**
 * @file
 * @brief HiCache target storage directory 实现。
 */
#include "markov/trace_graph/modules/hicache/storage/storage_directory.hpp"

#include <algorithm>
#include <ranges>
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
 * @brief 确保 backend hash namespace 记录存在。
 *
 * backend record 表示“某 scope/hash 在 L3 backend 可读”，不要求该 page 已经在
 * radix tree 中 materialize。prefetch 可以先通过 hash 命中，再在 checkpoint/admission
 * 边界把可读 prefix 插入 host tree。
 */
HiCacheStorageBackendRecord & HiCacheStorageDirectory::ensure_backend_record(const std::string & cache_scope, const std::string & page_hash) {
    auto & record = records_by_storage_key_[storage_key(cache_scope, page_hash)];
    if (record.storage_key.empty()) {
        record.storage_key = storage_key(cache_scope, page_hash);
        record.cache_scope = normalized_scope(cache_scope);
        record.page_hash = page_hash;
        record.known_epoch = ++epoch_;
    }
    return record;
}

HiCacheStorageRecord & HiCacheStorageDirectory::ensure_record(const std::string & cache_scope, const std::string & page_id) {
    /* page record 是 target page id 维度的视图；如果 backend hash 已经可读，新
       page record 创建时会继承 readable，而不是重新等待一次 source observation。 */
    auto & record = records_by_page_[page_id];
    if (record.page_id.empty()) {
        record.page_id = page_id;
        record.cache_scope = cache_scope.empty() ? page_scope_from_id(page_id) : normalized_scope(cache_scope);
        record.page_hash = page_hash_from_id(page_id);
        record.storage_key = storage_key(record.cache_scope, record.page_hash);
        record.known_epoch = ++epoch_;
    }
    auto & backend = ensure_backend_record(record.cache_scope, record.page_hash);
    if (backend.readable && !record.readable) {
        record.readable = true;
        record.readable_source = backend.readable_source;
        record.readable_epoch = backend.readable_epoch;
    }
    return record;
}

HiCacheStorageRecord & HiCacheStorageDirectory::ensure_record(const HiCacheProjectedPage & page) {
    auto & record = ensure_record(page.cache_scope, page.id);
    if (!page.hash.empty() && record.page_hash != page.hash) {
        record.page_hash = page.hash;
        record.storage_key = storage_key(record.cache_scope, record.page_hash);
    }
    auto & backend = ensure_backend_record(record.cache_scope, record.page_hash);
    if (backend.readable && !record.readable) {
        record.readable = true;
        record.readable_source = backend.readable_source;
        record.readable_epoch = backend.readable_epoch;
    }
    return record;
}

void HiCacheStorageDirectory::mark_backend_readable(HiCacheStorageBackendRecord & record, const std::string & source) {
    if (!record.readable) record.readable_epoch = ++epoch_;
    record.readable = true;
    if (!source.empty()) record.readable_source = source;
}

void HiCacheStorageDirectory::mark_record_readable(HiCacheStorageRecord & record, const std::string & source) {
    if (!record.readable) record.readable_epoch = ++epoch_;
    record.readable = true;
    if (!source.empty()) record.readable_source = source;
    auto & backend = ensure_backend_record(record.cache_scope, record.page_hash);
    mark_backend_readable(backend, record.readable_source);
}

void HiCacheStorageDirectory::observe_page(const HiCacheProjectedPage & page) {
    auto & record = ensure_record(page);
    record.known = true;
}

void HiCacheStorageDirectory::observe_path(const HiCachePagePath & path) {
    std::ranges::for_each(path.pages, [&](const auto & page) { observe_page(page); });
}

void HiCacheStorageDirectory::seed_readable_path(const HiCachePagePath & path, const std::string & source) {
    /* seed_readable_path 用于模型自己已经确认的 target page path，例如 write-back/
       write-through commit 后的 L3 readable。 */
    std::ranges::for_each(path.pages, [&](const auto & page) {
        auto & record = ensure_record(page);
        record.known = true;
        mark_record_readable(record, source);
    });
}

void HiCacheStorageDirectory::seed_readable_hashes(const std::string & cache_scope, const std::vector<std::string> & page_hashes, const std::string & source) {
    /* storage hash fact 只给 backend namespace 加可读性。若对应 target page 已经
       被投影出来，同步更新 page record；尚未投影的 hash 保留为 backend-only hit。 */
    std::ranges::for_each(page_hashes, [&](const auto & page_hash) {
        auto & backend = ensure_backend_record(cache_scope, page_hash);
        mark_backend_readable(backend, source);
        std::ranges::for_each(records_by_page_ | std::views::values, [&](auto & record) {
            if (record.storage_key == backend.storage_key) mark_record_readable(record, source);
        });
    });
}

void HiCacheStorageDirectory::mark_readable_pages(const std::string & cache_scope, const std::vector<std::string> & page_ids) {
    std::ranges::for_each(page_ids, [&](const auto & page_id) {
        auto & record = ensure_record(cache_scope, page_id);
        record.known = true;
        mark_record_readable(record, "modeled_storage_commit");
    });
}

void HiCacheStorageDirectory::mark_materialized_pages(const std::vector<std::string> & page_ids, HiCacheNodeId node_id) {
    /* materialized 表示 storage/host prefix 已经落到 radix node 上。它不改变
       backend readable 语义，只记录 page id 到 canonical tree node 的关联。 */
    std::ranges::for_each(page_ids, [&](const auto & page_id) {
        auto & record = ensure_record("", page_id);
        record.known = true;
        if (!record.materialized_node) record.materialized_epoch = ++epoch_;
        record.materialized_node = node_id;
        auto & backend = ensure_backend_record(record.cache_scope, record.page_hash);
        backend.materialized_pages.insert(page_id);
    });
}

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
    /* SGLang storage hit 只接受连续 prefix；中间断点之后的 backend readable page
       不能被本次 prefetch 当成可用结果。 */
    std::vector<std::string> prefix;
    prefix.reserve(pages.size());
    for (const auto & page : pages) {
        if (!readable(page)) break;
        prefix.push_back(page.id);
    }
    return prefix;
}

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

} // namespace markov::trace_graph::modules::hicache::storage
