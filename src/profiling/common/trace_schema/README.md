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

`radix_sim` consumes only model-input events with:

- `args.model_input: true`
- `args.domain: "cache_io"`
- `args.event_kind: "radix_op" | "storage_op" | "cache_operation"`

Observed movement/control events such as `HiCache::prefetch_l3_to_l2`,
`HiCache::load_l2_to_l1`, and `HiCache::transfer_kv_dim_exchange` may still be
emitted for event-level comparison, but they are debug facts unless
`model_input=true`.

Common args:

- `framework`
- `request_id`
- `operation_id`
- `op_id`
- `node_id`
- `event_kind`
- `tier_src`
- `tier_dst`
- `direction`
- `pool`
- `pool_name`
- `transfer_scope`
- `num_tokens`
- `num_pages`
- `page_size`
- `page_identity_kind`
- `trace_page_block_keys_hash`
- `runtime_page_keys_hash`
- `bytes_per_page`
- `bytes`
- `page_keys_hash`
- `key_truncated`
- `layout`
- `io_backend`
- `storage_backend`
- `write_policy`
- `status`

## Model Input Events

HiCache policy simulation consumes three semantic event kinds.

### `radix_op`

- `name: "HiCache::radix_op"`
- `args.event_kind: "radix_op"`

`radix_op` records workload/cache facts before modeling changes any policy. It
does not claim a physical transfer happened. Required args for strategy replay:

- `method`
- `op_seq`
- `cache_id`
- `page_size`
- `raw_token_len`
- `aligned_token_len`
- `dropped_tail_tokens`
- `node_id`
- `parent_node_id`
- `node_key_len`
- `hit_count`
- `backuped`
- `evicted`
- `node_local_block_keys_hash`
- `full_path_block_keys_hash`
- `parent_full_path_block_keys_hash`
- `block_key_truncated`
- `block_size_tokens`

`full_path_block_keys_hash` is the canonical block-level path identity.
`trace_page_block_keys_hash` maps the trace page size onto those blocks using
`b0,b1|b2,b3` formatting. Runtime/storage `page_keys_hash` is only an observed
alias and must not be used as the canonical scenario identity. `node_local_block_keys_hash`
and `parent_full_path_block_keys_hash` are retained for radix-tree validation.
The default block size is 32 tokens, and scenario page sizes must be divisible by
that block size for exact radix replay.

### `storage_op`

- `name: "HiCache::storage_op"`
- `args.event_kind: "storage_op"`

Storage events describe L3 backend facts. Required args:

- `operation_id`
- `request_id`
- `method=batch_exists|batch_get|batch_set`
- `page_size`
- `page_identity_kind=block_tuple`
- `trace_page_block_keys_hash`
- `page_keys_hash`
- `queried_pages`
- `hit_pages`
- `miss_pages`
- `success_pages`
- `storage_backend`
- `status`

For `batch_exists`, `page_keys_hash` and `trace_page_block_keys_hash` describe
the queried pages, while `hit_pages` / `miss_pages` describe the outcome. For
successful `batch_get` / `batch_set`, they describe the successful pages. Their
lengths must match one-to-one; otherwise the event is rejected as model input.

### `cache_operation`

- `name: "HiCache::cache_operation"`
- `args.event_kind: "cache_operation"`

Cache operation events link runtime lifecycle stages. Required args:

- `operation_id`
- `request_id`
- `operation_kind=prefetch|load|write`
- `stage=created|queued|started|completed|failed`
- `node_id`
- `full_path_block_keys_hash` or `page_keys_hash`
- `trace_page_block_keys_hash`
- `num_tokens`
- `num_pages`
- `status`

`operation_id` connects `prefetch`, storage query/read, host insertion, and
`load_back`. `radix_sim` rejects storage reads or load-backs that cannot
be linked to their lifecycle operation.
