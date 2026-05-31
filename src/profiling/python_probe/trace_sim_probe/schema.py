HICACHE_CATEGORY = "hicache"
FRAMEWORK_SGLANG = "sglang"

HICACHE_MATCH = "HiCache::match"
HICACHE_PREFETCH_QUERY = "HiCache::prefetch_query"
HICACHE_PREFETCH_L3_TO_L2 = "HiCache::prefetch_l3_to_l2"
HICACHE_LOAD_L2_TO_L1 = "HiCache::load_l2_to_l1"
HICACHE_BACKUP_L1_TO_L2 = "HiCache::backup_l1_to_l2"
HICACHE_WRITE_L2_TO_L3 = "HiCache::write_l2_to_l3"
HICACHE_EVICT_L1 = "HiCache::evict_l1"
HICACHE_EVICT_L2 = "HiCache::evict_l2"
HICACHE_TRANSFER_KV_DIM_EXCHANGE = "HiCache::transfer_kv_dim_exchange"

COMMON_HICACHE_ARGS = (
    "framework",
    "request_id",
    "op_id",
    "tier_src",
    "tier_dst",
    "direction",
    "pool",
    "num_tokens",
    "num_pages",
    "page_size",
    "bytes",
    "layout",
    "io_backend",
    "storage_backend",
    "write_policy",
    "status",
)
