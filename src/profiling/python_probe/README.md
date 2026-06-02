# Python Probe

Non-invasive Python runtime instrumentation for semantic events that are not
visible from LD_PRELOAD alone.

Enable it by putting this directory on `PYTHONPATH` and setting:

```bash
TRACE_SIM_PYTHON_PROBE=1
TRACE_SIM_PYTHON_PROBES=sglang.hicache,sglang.kvcacheio
TRACE_SIM_PYTHON_PROBE_OUTPUT=<trace-dir>
TRACE_SIM_PYTHON_PROBE_KEY_MODE=none|hash|block_hash|raw
TRACE_SIM_PYTHON_PROBE_BLOCK_SIZE_TOKENS=32
TRACE_SIM_PYTHON_PROBE_MAX_KEYS_PER_EVENT=4096
```

`sitecustomize.py` installs an import hook only when the probe is enabled. The
default probe set wraps SGLang HiCache/cache-controller methods and
`sgl_kernel.kvcacheio` transfer helpers, then emits Chrome Trace JSON with
`cat: "hicache"`.

`TRACE_SIM_PYTHON_PROBE_KEY_MODE` defaults to `none`. Use `block_hash` for
HiCache RadixCache simulation: it records fixed-size token-block identities so a
host-side model can rebuild pages under page-size scenarios such as 32, 64, and
128 tokens. The default block size is 32 tokens. Use `raw` only for local
debugging because it may expose
request/cache key material.

The SGLang HiCache probe emits model-input events and debug events.
`radix_sim` consumes only `model_input=true`, `domain=cache_io` events from:

- `HiCache::radix_op`
- `HiCache::storage_op`
- `HiCache::cache_operation`

Observed movement/control/query spans remain useful for event-level comparison,
but they are marked `model_input=false` and are not consumed by `radix_sim`.

The canonical page identity is the block tuple encoded in
`trace_page_block_keys_hash`, with `page_identity_kind="block_tuple"`.
SGLang runtime/storage `page_keys_hash` values are recorded as aliases for
event-level comparison only. The probe preserves valid zero ids such as
`operation_id=0`, `node_id=0`, and `parent_node_id=0` by serializing them as the
string `"0"`.

The probe does not modify `third_party/sglang` source. Missing target modules are
ignored so the same package can be present in non-SGLang runs.
