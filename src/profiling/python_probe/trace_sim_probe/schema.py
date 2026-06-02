HICACHE_CATEGORY = "hicache"
FRAMEWORK_SGLANG = "sglang"
HICACHE_RADIX_SCHEMA = "hicache_radix"

HICACHE_MATCH = "HiCache::match"
HICACHE_PREFETCH_QUERY = "HiCache::prefetch_query"
HICACHE_PREFETCH_L3_TO_L2 = "HiCache::prefetch_l3_to_l2"
HICACHE_LOAD_L2_TO_L1 = "HiCache::load_l2_to_l1"
HICACHE_BACKUP_L1_TO_L2 = "HiCache::backup_l1_to_l2"
HICACHE_WRITE_L2_TO_L3 = "HiCache::write_l2_to_l3"
HICACHE_EVICT_L1 = "HiCache::evict_l1"
HICACHE_EVICT_L2 = "HiCache::evict_l2"
HICACHE_TRANSFER_KV_DIM_EXCHANGE = "HiCache::transfer_kv_dim_exchange"
HICACHE_RADIX_OP = "HiCache::radix_op"
HICACHE_STORAGE_OP = "HiCache::storage_op"
HICACHE_CACHE_OPERATION = "HiCache::cache_operation"
HICACHE_DEBUG_EVENT = "HiCache::debug"

COMMON_HICACHE_ARGS = (
    "schema_version",
    "model_input",
    "framework",
    "request_id",
    "operation_id",
    "op_id",
    "producer",
    "domain",
    "event_kind",
    "tier_src",
    "tier_dst",
    "direction",
    "pool",
    "num_tokens",
    "num_pages",
    "page_size",
    "page_identity_kind",
    "trace_page_block_keys_hash",
    "runtime_page_keys_hash",
    "bytes",
    "layout",
    "io_backend",
    "storage_backend",
    "write_policy",
    "status",
    "method",
    "op_seq",
    "cache_id",
    "raw_token_len",
    "aligned_token_len",
    "dropped_tail_tokens",
    "parent_node_id",
    "node_id",
    "node_key_len",
    "hit_count",
    "backuped",
    "evicted",
    "block_keys_hash",
    "node_local_block_keys_hash",
    "full_path_block_keys_hash",
    "parent_full_path_block_keys_hash",
    "block_key_truncated",
    "block_size_tokens",
    "operation_kind",
    "stage",
    "queried_pages",
    "hit_pages",
    "miss_pages",
    "success_pages",
    "storage_success_pages",
    "storage_hit_tokens",
    "loaded_from_storage_tokens",
)
