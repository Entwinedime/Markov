# Project Structure

The current structure follows the migrated hook framework and trace graph work,
while leaving room for ktransformers and sglang specific adapters.

## Profiling

- `third_party/sglang/`: editable sglang fork submodule used by the runtime container.
- `third_party/ktransformers/`: editable ktransformers fork submodule used by the runtime container.
- `src/profiling/common/trace_schema/`: shared Chrome Trace event contract across producers.
- `src/profiling/native_hook/`: C++ LD_PRELOAD hook library.
- `src/profiling/native_hook/targets/`: framework/runtime-specific hook wrappers.
- `src/profiling/python_probe/`: env-gated Python import-hook/monkey-patch instrumentation, currently including SGLang HiCache probes.
- `scripts/build.sh`: builds framework images and matching hook libraries.
- `scripts/run.sh`: runs ad-hoc commands in framework runtime containers.
- `scripts/profile.sh`: runs JSON-configured profiling experiments.
- `scripts/lib/common.sh`: shared shell helpers for public script entrypoints.
- `scripts/internal/profile_runner.py`: container-side profile runner for server, workload, hook, and SGLang profile API orchestration.
- `scripts/internal/frameworks/sglang/`: SGLang source-install internals used by the runtime image.
- `scripts/internal/frameworks/ktransformers/`: ktransformers source-install internals used by the runtime image.
- `scripts/internal/hooks/`: container-side hook build helper.
- `scripts/trace/`: host-side trace merge and HiCache inspection tools.

Profiling is the containerized stage. SGLang and ktransformers use separate
Docker images, profile scripts, hook build directories, and `libhook.so`
outputs, all wired through the single `docker/compose/inference.yml` file.

## Modeling

- `src/modeling/trace_graph/include/trace_graph/core/`: Chrome Trace records, parser, DAG primitives, and existing Ascend DAG pipeline.
- `src/modeling/trace_graph/include/trace_graph/frontend/`: trace normalization and model config parsing.
- `src/modeling/trace_graph/include/trace_graph/domains/ascend_sync/`: boundary for existing Ascend event/sync/HCCL enrichment.
- `src/modeling/trace_graph/include/trace_graph/domains/cache_io/`: HiCache/KV-cache IO replay model and summary output.
- `src/modeling/trace_graph/include/trace_graph/simulation/`: topological replay boundary.
- `src/modeling/trace_graph/include/trace_graph/optimization/`: what-if transform boundary.
- `src/modeling/trace_graph/`: CLI wrapper and compatibility headers for the modeling engine.
- `configs/modeling/`: host-side model replay and what-if configs.
- `data/profile_runs/`: generated container-side profile run outputs.
- `data/traces/raw/`: host-side raw trace staging area.
- `data/traces/merged/`: merged Chrome Trace files used as DAG input.
- `data/traces/dag/`: generated DAG/simulation timeline JSON.
- `tests/fixtures/cache_io/`: synthetic Chrome Trace inputs for cache IO replay checks.
- `tests/run_trace_graph_fixtures.py`: host-side regression runner for cache IO modeling fixtures.
- `docs/hicache_validation.md`: current HiCache calibration workflow, acceptance criteria, and what-if roadmap.

The `cache_io` domain is an experimental replay model. It is wired into the
normal TraceGraph workflow so HiCache events are not a plugin-shaped outlier,
but current summaries should be treated as plumbing/sanity output until
bytes-per-page and KV layout inference are calibrated. Merge reports and
HiCache inspection reports live beside each run under
`data/profile_runs/<framework>/<run-id>/trace/merged/` and
`data/profile_runs/<framework>/<run-id>/model/`.

## Optimization

- `src/modeling/trace_graph` exposes what-if scaling through `--scale` and domain modeling through `--model-config`.
- `data/traces/dag/` stores generated what-if outputs.

Modeling and optimization are host-side stages by default. They consume trace
artifacts emitted from the runtime container and do not need the inference
frameworks to be running.

## Runtime

- `docker/images/base/sglang/`: stable SGLang Ubuntu/CANN/PyTorch environment image.
- `docker/images/base/ktransformers/`: stable ktransformers Ubuntu/CANN/PyTorch/torch_npu environment image.
- `docker/images/sglang/`: SGLang source-install runtime image based on the SGLang environment image.
- `docker/images/ktransformers/`: ktransformers source-install runtime image based on the ktransformers environment image.
- `docker/compose/inference.yml`: device-aware compose services for inference runs.
- `scripts/init_submodules.sh`: initializes framework submodules recursively.
- `scripts/build.sh`: builds the frontend environment image, runtime image, and hook; pass `--skip-env` to rebuild only the source-install runtime layer.
- `scripts/run.sh`: runs a custom command in the framework runtime image.
- `scripts/profile.sh`: runs repeatable profiling experiments from JSON configs.
