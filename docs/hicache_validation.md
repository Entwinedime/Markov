# HiCache Validation Protocol

This document defines the current SGLang + HiCache validation loop. The only
HiCache strategy path is:

```text
Python probe facts -> RadixSim -> what-if / fit matrix
```

Old profile data is not used as evidence after probe/model changes. Rerun the
profile matrix whenever the HiCache probe contract or RadixSim semantics change.

## RadixSim Contract

The Python probe emits current `hicache_radix` facts:

- `radix_op`: radix tree operations with full block path, parent path, node id,
  page size, and token lengths.
- `storage_op`: storage backend query/read/write facts with runtime page aliases
  and `trace_page_block_keys_hash`.
- `cache_operation`: lifecycle facts linking prefetch/load/write stages through
  `operation_id`.

RadixSim consumes only `model_input=true`, `domain=cache_io` events from those
three kinds. Runtime `page_keys_hash` is an observed alias; canonical page
identity is `page_identity_kind=block_tuple` plus `trace_page_block_keys_hash`.
If page identity, parent chain, operation lifecycle, or rank scope does not
close, RadixSim fails and writes `input_readiness.json`.

## Profile Matrix

Use the matrix config:

```bash
configs/experiments/sglang_qwen3_32b_hicache_tp2_calibration_matrix.json
```

Fixed assumptions:

- model: `/models/Qwen3-32B`
- TP: `2`
- storage backend: `file`
- key mode: `TRACE_SIM_PYTHON_PROBE_KEY_MODE=block_hash`
- block size: `TRACE_SIM_PYTHON_PROBE_BLOCK_SIZE_TOKENS=32`

Dry run:

```bash
scripts/profile.sh \
  configs/experiments/sglang_qwen3_32b_hicache_tp2_calibration_matrix.json \
  --dry-run
```

Real run:

```bash
scripts/profile.sh \
  configs/experiments/sglang_qwen3_32b_hicache_tp2_calibration_matrix.json
```

After a run, inspect each profile root:

```bash
python3 scripts/trace/inspect_hicache.py \
  data/profile_runs/sglang/hicache_calibration_matrix/<run-id> \
  --output data/profile_runs/sglang/hicache_calibration_matrix/<run-id>/model/hicache_inspect.json
```

The important readiness fields are `page_identity_map_ready`,
`runtime_page_alias_ready`, `operation_lifecycle_ready`,
`load_back_link_ready`, `parent_prefix_ready`, `state_scope_ready`, and
`radix_sim_ready`.

## What-If And Fit

Run explicit scenarios on one base trace:

```bash
python3 scripts/modeling/hicache_whatif.py \
  --suite configs/modeling/hicache_qwen3_tp2_whatif.json \
  --trace data/profile_runs/sglang/<run-id>/trace/merged/merged_trace.pid417.json \
  --trace data/profile_runs/sglang/<run-id>/trace/merged/merged_trace.pid418.json \
  --output-dir data/profile_runs/sglang/<run-id>/whatif
```

Run the calibration fit matrix:

```bash
python3 scripts/modeling/hicache_fit_matrix.py \
  --run-root data/profile_runs/sglang/hicache_calibration_matrix \
  --matrix-config configs/experiments/sglang_qwen3_32b_hicache_tp2_calibration_matrix.json \
  --output-dir data/profile_runs/sglang/hicache_calibration_matrix/fit \
  --pair-workers 4 \
  --overwrite
```

For a quick c00 row:

```bash
python3 scripts/modeling/hicache_fit_matrix.py \
  --run-root data/profile_runs/sglang/hicache_calibration_matrix \
  --matrix-config configs/experiments/sglang_qwen3_32b_hicache_tp2_calibration_matrix.json \
  --output-dir data/profile_runs/sglang/hicache_calibration_matrix/fit_c00 \
  --base c00_baseline \
  --pair-workers 4 \
  --cache-only-sidecar \
  --overwrite
```

Per-scenario `radix_sim_trace.json` is an intermediate artifact and is deleted
unless `--keep-generated-trace-output` is set.

## Acceptance

- Every profile run has Python probe sidecar events.
- Native merge reports have unmatched count `0`.
- Inspect reports `radix_sim_ready=true`.
- `load_back_pages_missing=0` in RadixSim summaries.
- Event-level diff aligns observed storage reads with generated `L3->L2` pages
  and observed load-back with generated `L2->L1` pages.
- c00->c01, c00->c02, and c00->c09 produce explainable page-size/write-policy
  deltas before running the full matrix.

## Regression Checks

```bash
python3 -m py_compile \
  scripts/bench/hicache_phased_workload.py \
  scripts/modeling/hicache_fit_matrix.py \
  scripts/modeling/hicache_whatif.py \
  scripts/trace/*.py

python3 tests/run_hicache_radix_sim_fixtures.py
python3 tests/run_trace_graph_fixtures.py
```
