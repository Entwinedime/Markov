# HiCache Validation Flow

This document keeps the current HiCache work honest: the goal is to prove the
profiling and modeling loop is wired correctly before treating the model as a
quantitative predictor.

## Current Flow

1. Run an SGLang profile config with native hook, torch profiler, and Python
   probe enabled.
2. Merge traces with:

   ```bash
   python3 scripts/trace/merge_all_traces.py --root data/profile_runs/sglang/<run-id> --overwrite
   ```

   Each merged PID gets `merge_report.pid*.json` with native match counts and
   python probe sidecar counts.

3. Inspect semantic HiCache coverage:

   ```bash
   python3 scripts/trace/inspect_hicache.py \
     data/profile_runs/sglang/<run-id> \
     --output data/profile_runs/sglang/<run-id>/model/hicache_inspect.json
   ```

4. Run TraceGraph with a model config. For Qwen3-32B TP=2 calibration, create a
   run-local config that sets:

   ```json
   {
     "cache_io": {
       "model_config_path": "/models/Qwen3-32B/config.json",
       "tp_size": 2
     }
   }
   ```

   TraceGraph then infers bytes per KV page from page size, layers,
   key-value heads per rank, head dim, and dtype bytes.

## Synthetic Fixtures

Run:

```bash
python3 tests/run_trace_graph_fixtures.py
```

These fixtures validate deterministic cache replay and byte accounting from
known Chrome Trace inputs. They are the first regression line for future
cache_io changes.

## Current Calibration Status

The latest smoke run uses:

- config: `configs/experiments/sglang_qwen3_32b_hicache_tp2_smoke.json`
- run dir: `data/profile_runs/sglang/manual_hicache_tp2_smoke`
- model: `/models/Qwen3-32B`
- TP size: 2

Acceptance for this stage:

- benchmark requests complete successfully
- torch, native hook, and python probe traces exist for both ranks
- merge reports show all native events matched
- HiCache inspection finds nonzero semantic events
- TraceGraph emits a cache_io summary with nonzero transfer events
- calibrated bytes are nonzero for events that expose page counts

Known limitation:

- many HiCache events are control/status checks and do not carry page counts
- `bytes_by_edge` may be partially zero until probe coverage is expanded for
  every real movement path
- the current summary is a calibration signal, not a final correctness claim

## What-If Roadmap

Only after bytes/page coverage and synthetic fixtures are stable should
what-if experiments become the focus:

- L1/L2/L3 capacity sweeps
- per-tier bandwidth and latency sweeps
- prefetch policy variants
- eviction policy variants
- storage backend comparison once event coverage is comparable
