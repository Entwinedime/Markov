# Trace Sim Model

Python object-modeling engines for policy simulation.

`hicache_radix_sim.py` is the HiCache strategy-prediction engine. It consumes
current `hicache_radix` model-input events from the Python probe:
`HiCache::radix_op`, `HiCache::storage_op`, and
`HiCache::cache_operation`.

The canonical page identity is `page_identity_kind="block_tuple"` plus
`trace_page_block_keys_hash`. Runtime/storage `page_keys_hash` is an observed
alias for event comparison only.

RadixSim rejects incomplete input instead of falling back to another model.
Tier and radix state are partitioned by trace `pid` and `cache_id`, so multiple
ranks with the same page identity do not share L1/L2/L3 residency.
