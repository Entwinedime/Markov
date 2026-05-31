# ktransformers / sglang Trace Simulation

This repository is organized around a trace-driven performance simulation flow for
ktransformers and sglang inference workloads:

1. Collect runtime traces with custom LD_PRELOAD hooks and framework profilers.
2. Merge custom CPU hook traces with profiler Chrome Trace output.
3. Build a dependency DAG from the merged trace.
4. Run topological simulation and graph-transform what-if analysis.

The current codebase integrates two existing pieces of progress:

- `src/profiling/native_hook/`: C++ LD_PRELOAD hook framework for custom trace events.
- `src/profiling/common/trace_schema/`: shared Chrome Trace event contract.
- `src/profiling/python_probe/`: env-gated Python runtime probes for semantic events such as SGLang HiCache.
- `src/modeling/trace_graph/`: C++ trace parser, DAG builder, domain models, simulator, and what-if CLI.
- `third_party/sglang/`: Entwinedime's sglang fork, tracked as a Git submodule.
- `third_party/ktransformers/`: Entwinedime's ktransformers fork, tracked as a Git submodule.

## Layout

```text
.
├── src/profiling/native_hook/   # LD_PRELOAD hook framework and AscendCL hook targets
├── src/profiling/common/        # Shared Chrome Trace event schema
├── src/profiling/python_probe/  # Non-invasive Python runtime probes
├── src/modeling/trace_graph/    # Trace -> normalized DAG -> domain simulation engine
├── third_party/sglang/          # sglang fork submodule
├── third_party/ktransformers/   # ktransformers fork submodule
├── scripts/build.sh             # Build runtime image and matching hook
├── scripts/run.sh               # Run one-off commands in framework containers
├── scripts/profile.sh           # Run JSON-configured profiling experiments
├── scripts/lib/                 # Shared shell helpers for public entrypoints
├── scripts/internal/            # Docker/profile/build internals, not user entrypoints
├── scripts/trace/               # Trace merge tools
├── docker/images/base/sglang/   # SGLang stable Ubuntu/CANN/PyTorch environment
├── docker/images/base/ktransformers/
│                                  # ktransformers stable Ubuntu/CANN/PyTorch environment
├── docker/images/sglang/        # SGLang Ubuntu runtime image
├── docker/images/ktransformers/ # ktransformers Ubuntu runtime image
├── docker/compose/inference.yml # Compose services for runtime profiling
├── configs/experiments/         # JSON profile experiment suites
├── configs/modeling/            # Host-side model replay / what-if configs
├── data/profile_runs/           # Generated profile run directories
├── data/traces/                 # Host-side raw, merged, and DAG trace artifacts
└── docs/                        # Design notes by stage
```

## Submodules

The inference frameworks are intentionally kept as editable submodules because
profiling and runtime instrumentation may require source changes:

```bash
git submodule update --init --recursive
```

The configured remotes are:

- `https://github.com/Entwinedime/sglang.git`
- `https://github.com/Entwinedime/ktransformers.git`

## Host Build

```bash
cmake -S . -B build -DHOOK_ENABLE_PAPI=OFF
cmake --build build -j
```

The host build is primarily for modeling and what-if analysis:

- `build/bin/trace_graph`

Do not rely on the host-built `libhook.so` for profiling. The hook library is
compiled inside the Docker runtime environment so it matches the framework
container, toolchain, and mounted runtime dependencies.

## Hook Profiles

The hook framework supports separate profiles:

- `sglang`: builds `build/docker/sglang/lib/libhook.so`
- `ktransformers`: builds `build/docker/ktransformers/lib/libhook.so`
- `ascendcl`: generic AscendCL wrapper profile
- `template`: empty hook template

The framework build scripts call this inside the container:

```bash
scripts/internal/hooks/build.sh sglang
scripts/internal/hooks/build.sh ktransformers
```

## Docker / Ubuntu

The Docker setup is split into two layers per frontend:

- environment images under `docker/images/base/<framework>/` hold the slower,
  mostly stable Ubuntu/CANN/PyTorch/torch_npu dependency stack.
- runtime images under `docker/images/<framework>/` are based on those
  environment images and install the current framework fork source.

SGLang and ktransformers may need different Python environments, launch
commands, and hook targets.

Runtime profiling runs in Docker. DAG construction, simulation, and what-if
analysis can run on the host through `build/bin/trace_graph`.

Both runtime frontends live in one compose file. Build the images with:

```bash
scripts/build.sh sglang --image-only
scripts/build.sh ktransformers --image-only
```

When only framework source changed and the environment image is already current,
skip the first layer:

```bash
scripts/build.sh ktransformers --skip-env --image-only
```

Open an interactive shell with:

```bash
scripts/run.sh sglang -- bash
scripts/run.sh ktransformers -- bash
```

Build the runtime image and matching hook library together:

```bash
scripts/build.sh sglang
scripts/build.sh ktransformers
```

The SGLang image follows the Ascend NPU source-install flow rather than using a
prebuilt SGLang serving image:

- base image: `quay.io/ascend/cann:8.5.0-910b-ubuntu22.04-py3.11` for Atlas 800I A2
- Python runtime: 3.11 from the CANN image
- dependencies: `torch==2.10.0`, Ascend `torch_npu==2.10.0`,
  `triton_ascend==3.2.1`, `memfabric-hybrid==1.0.8`, and
  `sgl-kernel-npu` release wheels installed in the environment image
- source install: the runtime image runs `scripts/internal/frameworks/sglang/install_from_source.sh`,
  which copies
  `third_party/sglang/python/pyproject_npu.toml` over `pyproject.toml`, then
  installs SGLang editable from the copied fork with `.[all_npu]`

The ktransformers image follows the Ascend NPU tutorial flow, but keeps it on an
Ubuntu CANN base instead of the tutorial's openeuler MindIE image:

- base image: `quay.io/ascend/cann:8.3.rc1-910b-ubuntu22.04-py3.11`
- Python runtime: 3.11 from the CANN image
- dependencies: `torch==2.5.1`, `torchvision==0.20.1`, `torchaudio==2.5.1`,
  `numpy==1.26.4`, `transformers==4.57.1`, and the build/runtime packages used by
  the Ascend NPU path
- `torch_npu` is cloned from `https://gitcode.com/Ascend/pytorch.git` at
  branch `v2.5.1` and compiled from source during the environment image build.
  The image installs only the wheel produced by that local source build and
  strips any `+git...` suffix from `torch_npu/version.py`
- source install: the runtime image runs `scripts/internal/frameworks/ktransformers/install_from_source.sh`
  and installs the copied `third_party/ktransformers/archive` runtime with
  `USE_BALANCE_SERVE=1`, patches the archive config to `attn.page_size=128` and
  `attn.chunk_size=16384`, runs upstream `install.sh --dev cuda` so
  `custom_flashinfer` follows the path that currently builds successfully, then
  verifies the image-built `torch_npu` environment

For SGLang profiling runs that need LD_PRELOAD hooks and SGLang's
`/start_profile` / `/stop_profile` APIs, use a JSON experiment config instead:

```bash
scripts/profile.sh configs/experiments/sglang_qwen3_32b_profile_smoke.json
```

The profile config owns the server command, workload command, hook library,
hook trace path, SGLang profiler request body, and per-run environment. Compose
only provides the container, devices, and mounts.

Profile configs can describe either one run or a suite. A suite keeps common
settings at the top level and adds an `experiments` list; each experiment
deep-merges over the common settings and is written under one suite directory:

```json
{
  "name": "qwen3-32b-profile-smoke",
  "framework": "sglang",
  "server": {"command": ["python3", "-m", "sglang.launch_server", "..."]},
  "bench": {"args": {"random_input_len": 64}},
  "experiments": [
    {"name": "out16", "bench": {"args": {"random_output_len": 16}}},
    {
      "name": "out32",
      "$unset": ["env.STREAMS_PER_DEVICE"],
      "bench": {"args": {"random_output_len": 32}}
    }
  ]
}
```

Inside an experiment, `$unset` removes inherited common fields before the run is
written. It accepts dot-separated paths such as `env.STREAMS_PER_DEVICE` or
`profile.profile_stages`.

The Dockerfiles and compose services do not set an entrypoint. They default to a
bash shell. `scripts/build.sh` builds images and hooks, `scripts/run.sh` opens
ad-hoc framework containers, and `scripts/profile.sh` runs profiling
experiments.

The compose services bind-mount:

- the repository workspace for scripts, hook sources, trace outputs, and configs
- Ascend driver/runtime device paths
- model directories under `/models` and `/root/models`
- generated profile outputs under `data/profile_runs/`

To add another inference framework later, add a new `docker/images/<framework>/`
image, `scripts/internal/frameworks/<framework>/` source-install helper, hook
profile/target if needed, and a service in `docker/compose/inference.yml`. Then
extend `scripts/build.sh`, `scripts/run.sh`, `scripts/profile.sh`, and
`scripts/lib/common.sh` by framework name.

Framework submodules are copied into the second Docker layer and installed into
the runtime image. Source edits on the host require rebuilding that runtime
layer, usually with `scripts/build.sh <framework> --skip-env`.

The compose runtime does not set `PYTHONPATH` to mounted framework sources.
Framework packages are installed into the runtime image during the second Docker
layer build; after source edits, rebuild that runtime layer instead of shadowing
installed packages from `/workspace`.

## Trace Processing

Merge custom hook traces with Ascend profiler traces:

```bash
python3 scripts/trace/merge_all_traces.py --root data/profile_runs/sglang/<suite-or-run-id> --overwrite
```

Each merged PID gets a sibling `merge_report.pid*.json` with native hook match
counts, sidecar append counts, and matching diagnostics. Inspect merged HiCache
events and field coverage with:

```bash
python3 scripts/trace/inspect_hicache.py \
  data/profile_runs/sglang/<run-id> \
  --output data/profile_runs/sglang/<run-id>/model/hicache_inspect.json
```

Build and simulate a DAG:

```bash
build/bin/trace_graph -o data/traces/dag/output_graph.json data/traces/merged/merged_trace.json
```

Apply a domain model, such as SGLang HiCache multi-level KV-cache replay:

```bash
build/bin/trace_graph \
  --model-config configs/modeling/hicache_ascend_file.json \
  --model-summary data/traces/dag/hicache_summary.json \
  -o data/traces/dag/hicache_dag.json \
  data/traces/merged/merged_trace.json
```

The HiCache `cache_io` model is currently experimental. A non-empty summary only
means the profile, merge, and replay plumbing worked; it is not yet proof that
the multi-level KV-cache model is quantitatively correct. For calibration runs,
set `cache_io.model_config_path` to the model's HuggingFace `config.json` and
`cache_io.tp_size` to the serving tensor-parallel size so TraceGraph can infer
bytes per KV page from model metadata. Synthetic fixtures can be checked with:

```bash
python3 tests/run_trace_graph_fixtures.py
```

`bytes_by_edge` may still be partially zero when SGLang events expose control
status but not concrete page counts. That is expected until the HiCache probe
coverage is expanded and validated against known storage traffic.

Run what-if scaling:

```bash
build/bin/trace_graph \
  --scale "CPUInfer::sync=0.5,aclrtMemcpyAsync=2.0" \
  -o data/traces/dag/what_if.json \
  data/traces/merged/merged_trace.json
```
