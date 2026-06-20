#pragma once

#include "trace_graph/modules/hicache/hicache_target_pager.hpp"
#include "trace_graph/modules/hicache/hicache_token_radix_tree.hpp"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace TraceGraph {

/**
 * @brief 单个 target page 的 storage backend 目录记录。
 *
 * 该记录描述 storage backend 对 page hash 的可读性；tree node 是否已经 materialize
 * 是另一个维度，不能把二者混成同一个状态。
 */
struct HiCacheStorageRecord {
    std::string page_id;
    std::string cache_scope;
    std::string page_hash;
    std::string storage_key;
    bool known = false;
    bool readable = false;
    std::string readable_source;
    uint64_t known_epoch = 0;
    uint64_t readable_epoch = 0;
    uint64_t materialized_epoch = 0;
    std::optional<HiCacheNodeId> materialized_node;
};

/**
 * @brief storage backend hash namespace 的记录。
 *
 * backend 目录可以早于 radix tree materialization 存在。SGLang storage hit query
 * 查询的是 hash/prefix key，而不是 host radix 中已经存在的 node。
 */
struct HiCacheStorageBackendRecord {
    std::string storage_key;
    std::string cache_scope;
    std::string page_hash;
    bool readable = false;
    std::string readable_source;
    uint64_t known_epoch = 0;
    uint64_t readable_epoch = 0;
    std::set<std::string> materialized_pages;
};

/**
 * @brief target storage namespace 的只读性目录。
 *
 * SGLang 的 storage hit query 先查询 backend hash，再把连续命中 prefix 写入 host
 * radix。该目录保留这个边界：storage readable 不要求 tree 中已经有 host/device node。
 */
class HiCacheStorageDirectory {
public:
    /** @brief 观察 target page identity，但不默认认为 storage readable。 */
    void observe_page(const HiCacheProjectedPage & page);

    /** @brief 观察一条 target page path。 */
    void observe_path(const HiCachePagePath & path);

    /** @brief 将一条 projected path 的 page hash seed 成 storage-readable。 */
    void seed_readable_path(const HiCachePagePath & path, const std::string & source);

    /** @brief 将一组 backend hash seed 成 storage-readable，不要求 tree 已有 node。 */
    void seed_readable_hashes(const std::string & cache_scope, const std::vector<std::string> & page_hashes, const std::string & source);

    /** @brief 标记一组 page 已经可以从 storage backend 读取。 */
    void mark_readable_pages(const std::string & cache_scope, const std::vector<std::string> & page_ids);

    /** @brief 记录一组 storage-readable page 已经 materialize 到 tree node。 */
    void mark_materialized_pages(const std::vector<std::string> & page_ids, HiCacheNodeId node_id);

    /** @brief 判断 page 是否 storage-readable。 */
    [[nodiscard]] bool readable(const std::string & page_id) const;

    /** @brief 判断 projected page 对应 backend hash 是否 storage-readable。 */
    [[nodiscard]] bool readable(const HiCacheProjectedPage & page) const;

    /** @brief 返回 planned path 中 storage-readable 的连续命中 prefix。 */
    [[nodiscard]] std::vector<std::string> contiguous_readable_prefix(const std::vector<std::string> & page_ids) const;

    /** @brief 返回 projected path 中 storage-readable 的连续命中 prefix。 */
    [[nodiscard]] std::vector<std::string> contiguous_readable_prefix(const std::vector<HiCacheProjectedPage> & pages) const;

    /** @brief 返回 storage-readable page id；可选择包含尚未 materialize 的 backend-only hash。 */
    [[nodiscard]] std::set<std::string> readable_page_ids(bool include_backend_only) const;

    /** @brief 返回所有目录记录，供派生视图或 debug 审计使用。 */
    [[nodiscard]] const std::unordered_map<std::string, HiCacheStorageRecord> & records() const { return records_by_page_; }

    /** @brief 返回 backend hash namespace 记录。 */
    [[nodiscard]] const std::unordered_map<std::string, HiCacheStorageBackendRecord> & backend_records() const { return records_by_storage_key_; }

    /** @brief 已观察 target page identity 数。 */
    [[nodiscard]] uint64_t known_page_count() const;

    /** @brief page 维度 storage-readable 数。 */
    [[nodiscard]] uint64_t readable_page_count() const;

    /** @brief backend hash namespace 维度 readable hash 数。 */
    [[nodiscard]] uint64_t backend_readable_count() const;

    /** @brief 已 materialize 到 radix tree 的 page 数。 */
    [[nodiscard]] uint64_t materialized_page_count() const;

private:
    uint64_t epoch_ = 0;
    std::unordered_map<std::string, HiCacheStorageRecord> records_by_page_;
    std::unordered_map<std::string, HiCacheStorageBackendRecord> records_by_storage_key_;

    [[nodiscard]] HiCacheStorageRecord & ensure_record(const std::string & cache_scope, const std::string & page_id);
    [[nodiscard]] HiCacheStorageRecord & ensure_record(const HiCacheProjectedPage & page);
    [[nodiscard]] HiCacheStorageBackendRecord & ensure_backend_record(const std::string & cache_scope, const std::string & page_hash);
    [[nodiscard]] static std::string storage_key(const std::string & cache_scope, const std::string & page_hash);
    void mark_record_readable(HiCacheStorageRecord & record, const std::string & source);
    void mark_backend_readable(HiCacheStorageBackendRecord & record, const std::string & source);
};

} // namespace TraceGraph
