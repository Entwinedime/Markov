# HiCache Validation Flow

This document keeps the current HiCache work honest: the goal is to prove the
profiling and modeling loop is wired correctly before treating the model as a
quantitative predictor.

## Current Flow

1. Run an SGLang profile config with native hook, torch profiler, and Python
   probe enabled. Use `TRACE_SIM_PYTHON_PROBE_KEY_MODE=hash` for base traces
   that will feed capacity, eviction, or prefetch-policy what-if replay.
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

4. Run TraceGraph with a model config. For Qwen3-32B TP=2 calibration, either
   use `configs/modeling/hicache_qwen3_tp2_whatif.json` or create a run-local
   config that sets:

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

5. Run explicit what-if scenarios without changing the base trace:

   ```bash
   python3 scripts/modeling/hicache_whatif.py \
     --suite configs/modeling/hicache_qwen3_tp2_whatif.json \
     --trace data/profile_runs/sglang/<run-id>/trace/merged/merged_trace.pid417.json \
     --trace data/profile_runs/sglang/<run-id>/trace/merged/merged_trace.pid418.json \
     --output-dir data/profile_runs/sglang/<run-id>/whatif
   ```

   The runner materializes one scenario config, `cache_io_summary.json`,
   `trace_graph.json`, and `run_summary.json` per scenario, then writes
   `whatif_summary.json` and `whatif_summary.csv`.

## Synthetic Fixtures

Run:

```bash
python3 tests/run_trace_graph_fixtures.py
```

These fixtures validate deterministic cache replay, movement/control separation,
byte accounting, capacity pressure, eviction-policy differences, bandwidth
sensitivity, and prefetch-policy replay from known Chrome Trace inputs. They are
the first regression line for future cache_io changes.

## Current Calibration Status

The latest smoke run uses:

- config: `configs/experiments/sglang_qwen3_32b_hicache_tp2_smoke.json`
- what-if base config: `configs/experiments/sglang_qwen3_32b_hicache_tp2_whatif_base.json`
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
- key coverage is nonzero when the what-if base config is used

Known limitation:

- many HiCache events are control/status checks and do not carry page counts
- `bytes_by_edge` may be partially zero when a real movement path still lacks
  pages/bytes metadata
- the current summary and what-if output are calibration signals, not final
  absolute-latency correctness claims

## What-If Scope

The current what-if layer is deterministic scenario replay only. It does not
search for an optimum and does not generate sweeps by itself. Supported knobs:

- L1/L2/L3 capacity changes
- per-tier bandwidth and latency changes
- `prefetch_policy = trace_replay | none`
- `eviction = lru | fifo | infinite`

Use `inspect_hicache.py` first. Its `whatif_readiness` section states whether a
trace has enough bytes/pages, page keys, and prefetch/load linkage for each
class of scenario.
