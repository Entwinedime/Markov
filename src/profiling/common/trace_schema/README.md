# Trace Event Contract

Profiling producers communicate with modeling through Chrome Trace style events.
Torch profiler events, native LD_PRELOAD hook events, and Python probe events are
merged before they enter TraceGraph.

## Producer Boundaries

- `native_hook` emits native runtime spans with `cat: "hook"`.
- `python_probe` emits runtime semantic spans with domain categories such as
  `cat: "hicache"`.
- Torch profiler output is treated as an external Chrome Trace producer.

Profiling records observed runtime facts only. It does not simulate, infer
policies, or mutate model state for what-if analysis.

## HiCache Events

HiCache events use `cat: "hicache"` and names under the `HiCache::` namespace:

- `HiCache::match`
- `HiCache::prefetch_query`
- `HiCache::prefetch_l3_to_l2`
- `HiCache::load_l2_to_l1`
- `HiCache::backup_l1_to_l2`
- `HiCache::write_l2_to_l3`
- `HiCache::evict_l1`
- `HiCache::evict_l2`
- `HiCache::transfer_kv_dim_exchange`

Common args:

- `framework`
- `request_id`
- `op_id`
- `tier_src`
- `tier_dst`
- `direction`
- `pool`
- `num_tokens`
- `num_pages`
- `page_size`
- `bytes`
- `layout`
- `io_backend`
- `storage_backend`
- `write_policy`
- `status`
