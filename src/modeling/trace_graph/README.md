# TraceGraph — Heterogeneous Trace Modeling & Simulation Engine

Converts Chrome Tracing JSON from Ascend NPU inference into a **dependency DAG**, then runs **topological simulation** and **what-if analysis** to predict end-to-end latency under hypothetical scenarios (e.g., faster CPU compute, larger data transfers).

## Overview

```
Chrome Trace JSON
  -> frontend normalization
  -> core DAG construction
  -> domain modeling: ascend_sync, cache_io
  -> simulation
  -> optimization / what-if output
```

The engine takes the flat, timestamp-ordered trace and recovers the **causal structure** that timestamps alone cannot express — CPU launch→NPU execution, cross-stream synchronization, multi-card communication barriers. It then discards original timestamps and replays the DAG topologically, producing a **predicted timeline** driven purely by node durations and dependency edges.

## File Structure

```
src/modeling/trace_graph/
├── CMakeLists.txt
├── .clang-format
├── include/trace_graph/
│   ├── core/                  # ActivityRecord, parser, DAG primitives
│   ├── frontend/              # trace normalization and model config parsing
│   ├── domains/               # ascend_sync and cache_io domain boundaries
│   ├── simulation/            # topological replay boundary
│   └── optimization/          # what-if transform boundary
├── src/
│   ├── core/
│   ├── frontend/
│   ├── domains/cache_io/
│   └── cli/main.cpp
└── build/bin/trace_graph       # Built binary from the top-level CMake build
```

Compatibility wrapper headers still exist directly under `include/trace_graph/`
so existing includes keep working while new code uses the stage-specific
subdirectories.

## Build

```bash
cmake -S . -B build
cmake --build build -j
```

Zero external dependencies. Requires only C++17 and CMake ≥ 3.16.

From the top-level repository, use:

```bash
cmake -S . -B build
cmake --build build -j
```

The binary is emitted as `build/bin/trace_graph`.

## CLI

```
trace_graph [OPTIONS] <trace_file> [trace_file ...]

Options:
  -h, --help                  Show this help
  -o, --output FILE           Output file (default: output_graph.json)
  -s, --scale NAME=FACTOR[,...]
                              Time-scaling operations (repeatable, comma-separated)
                              Example: --scale "CPUInfer::sync=0.5"
                              Example: --scale "aclrtMemcpyAsync=2.0,CPUInfer::sync=0.3"
  -d, --debug                 Export intermediate debug files
  -v, --verbose               Show progress output
  --full-output               Generate full Chrome tracing with edge flows
  --no-raw                    Skip raw parsed trace export
  --model-config FILE         Apply domain model config, e.g. cache_io tiers
  --model-summary FILE        Model summary JSON output
```

**Multiple trace files** are treated as separate GPU cards and automatically merged with HCCL communication edges.

All `--scale` values are applied together before a **single** simulation run.

Input can be either a top-level Chrome Trace event array or the standard
`{"traceEvents": [...]}` object shape.

## Pipeline

Everything starts in `TraceDAG::from_records()`:

### Step 0 — Deduplication

NPU traces often contain duplicate events (same `ts` + `dur`) from CPU and NPU mirrors. These are merged, keeping the NPU-side hardware metadata.

### Step 1–2 — Sort & Group

Events are sorted by timestamp and grouped by stream: each NPU stream gets its own group; all CPU events go into a single `CPU_MERGED` group.

### Step 3 — Leaf Extraction

Trace events are nested: `Node@launch` contains `Runtime@acl` which contains the actual `Kernel`. Only the **innermost execution nodes** matter for simulation.

A stack-based containment check identifies parent-child relationships. Framework wrappers (`Node@launch`, `Runtime@*`, enqueue nodes) are discarded. Their `correlation_id` is inherited downward so cross-device links are preserved.

### Step 4–5 — Graph Construction

Six substeps executed in strict order:

| Method | Edges Built |
|--------|-------------|
| `graphinit()` | Creates nodes, fills indexing maps (thread→nodes, correlation_id→nodes, etc.), classifies nodes by type |
| `addCorrelationEdges()` | **Correlation** — CPU launch → NPU execution, matched by `correlation_id` / `connection_id` |
| `addSequentialEdges()` | **Sequential** — within-stream ordering, plus `cpuinterval` (gap between CPU events) |
| `joinEvent()` | Preprocesses `EVENT_RECORD` nodes: builds `eventId→nodes` index, `rawstream→stream` mapping |
| `blockinganalysis()` | **Sync** — four sync patterns (see below) |
| `joinSync()` | Normalizes sync/wait node durations to 10 µs |

### Edge Types

| Type | Meaning | Built By |
|------|---------|----------|
| `Sequential` | Same-thread/stream ordering | `addSequentialEdges` |
| `Correlation` | CPU launch → NPU execution | `addCorrelationEdges` |
| `Sync` | Cross-stream / CPU-NPU synchronization | `blockinganalysis` |
| `HCCL` | Multi-card collective communication barrier | `merge_graphs` |

### Blocking Analysis — Four Sync Patterns

`blockinganalysis()` is the most hardware-specific part, modeling Ascend's synchronization primitives:

1. **StreamWaitEvent** — An `EVENT_WAIT` on one stream waits for the nearest prior `EVENT_RECORD` on a different stream (matched by event ID, located by binary search).
2. **Notify Wait** — A `NOTIFY_WAIT` waits for the nearest prior `NOTIFY_RECORD`.
3. **Model Execute** — During `MODEL_EXECUTE`, the first event on each NPU stream is forced to wait for the model to start.
4. **StreamSynchronize** — A CPU-side `StreamSynchronize` waits for the last launched event on the target stream (located by `lower_bound` on `launch_ts`).

## Simulation

```cpp
void TraceDAG::simulation() {
    // Kahn's algorithm — BFS topological traversal
    for each node u:
        complete_time[u] = node_duration[u]
        if critical_predecessor p exists:
            complete_time[u] += complete_time[p]
            if same CPU thread:  complete_time[u] += p.cpuinterval   // scheduling gap
            if same NPU stream:  complete_time[u] += 1.2 μs          // pipeline bubble
}
```

Original timestamps are **discarded entirely**. Time is derived solely from DAG topology and intrinsic node durations. Sync events (normalized to 10 µs in `joinSync`) no longer carry their original variable wait times — instead, wait time emerges from the dependency path length.

**Cycle detection**: if not all nodes are processed, a DFS tri-color traversal identifies and prints the exact cycle path with node names, timestamps, and edge types.

## Multi-Card Merge

```cpp
static TraceDAG merge_graphs(std::vector<TraceDAG>& graphs);
```

- Offsets node/edge IDs across graphs
- Groups HCCL kernels by name across GPUs  
- Aligns durations to the minimum within each group
- Adds **HCCL** cross-dependencies: each GPU's post-communication node waits for all other GPUs' communication nodes

## What-If Analysis

```cpp
dag.opt_scale("pattern", factor);   // scale matching node durations
dag.simulation();                   // re-run with new durations
```

The formula: `new_dur = max(ori_dur − 3000, 0) × factor + 3000`. The 3 µs threshold prevents scaling noise from trivial framework overhead events.

Via CLI:
```bash
# Predict effect of 2× faster CPU compute
trace_graph -s "CPUInfer::sync=0.5" trace.json

# Predict effect of 3× larger data transfers
trace_graph -s "aclrtMemcpyAsync=3.0" trace.json

# Combined scenario
trace_graph -s "CPUInfer::sync=0.5,aclrtMemcpyAsync=2.0" trace.json
```

## Cache IO Domain

When a model config enables `cache_io`, TraceGraph consumes HiCache events from
the Python probe and annotates matching DAG nodes with cache-domain timing. It
also writes a summary JSON with:

- hit tokens by tier
- bytes moved by tier edge
- resident pages by tier
- eviction/writeback counters
- estimated cache IO latency
- field coverage counters such as `events_with_pages`, `events_with_bytes`, and
  `missing_bytes_events`

Example:

```bash
build/bin/trace_graph \
  --model-config configs/modeling/hicache_ascend_file.json \
  --model-summary data/traces/dag/hicache_summary.json \
  -o data/traces/dag/hicache_dag.json \
  data/traces/merged/merged_trace.json
```

This domain is experimental. The first acceptance target is that HiCache events
flow through profile, merge, DAG construction, and summary generation. Quantitative
correctness is checked in layers:

1. Synthetic fixtures under `tests/fixtures/cache_io/` validate deterministic
   cache replay behavior and bytes-per-page math.
2. `scripts/trace/inspect_hicache.py` validates real-run event coverage before
   modeling.
3. Calibration configs can set `cache_io.model_config_path` and `cache_io.tp_size`
   so bytes per page are inferred from HuggingFace model metadata.

`bytes_by_edge` can still be partially zero when real HiCache events represent
control/status checks rather than concrete page movement. Treat this as a probe
coverage signal, not a correct final cache model.

Run the fixture smoke test from the repository root:

```bash
python3 tests/run_trace_graph_fixtures.py
```

## Output

`output_graph.json` contains a **dual timeline**:
- `pid` = original + `"000"` → raw trace timestamps
- `pid` = standalone number → simulated timestamps

Open in [Perfetto](https://ui.perfetto.dev) to visually compare original vs. predicted execution.

With `--full-output`, edge flow events are also exported, rendering dependency arrows in the trace viewer.

## Logger

All diagnostics go to stderr with timestamped, color-coded levels:

```
[INFO  16:18:06.545] Loading trace from: gpu0.json as GPU 0
[WARN  16:18:07.012] No records parsed for bad.json. Skipping.
[ERROR 16:18:07.234] No valid graphs built. Exiting.
[DEBUG 16:18:07.456] Exported leaf nodes to debug_1_leaf_nodes_0.json
```

Colors: green (INFO), yellow (WARN), bold red (ERROR), dim cyan (DEBUG). Automatically disabled when stderr is not a TTY. Force with `TRACE_GRAPH_COLOR=1`.

## Environment

| Variable | Effect |
|----------|--------|
| `DEBUG_TRACE=1` | Export intermediate DAG stages as JSON (set by `-d` flag) |
| `TRACE_GRAPH_COLOR=0` | Disable colored log output |
