# Runtime Boundary

The project has two execution zones.

## Docker Runtime Zone

Use Docker for inference framework execution and runtime instrumentation. There
is one compose file with one service per frontend:

- compose: `docker/compose/inference.yml`
- SGLang service: `sglang-profile`, image source `docker/images/sglang/Dockerfile`
- ktransformers service: `ktransformers-profile`, image source
  `docker/images/ktransformers/Dockerfile`

Mounted runtime inputs:

- repository workspace for scripts, hook sources, trace outputs, and configs
- Ascend driver/runtime device paths
- model directories under `/models` and `/root/models`
- common hook build helper: `scripts/hooks/build.sh`
- trace merge helpers: `scripts/trace`
- trace output: `data/profile_runs`

The framework build step compiles the matching `libhook.so` inside the container:

- `build/docker/sglang/lib/libhook.so`
- `build/docker/ktransformers/lib/libhook.so`

`scripts/build.sh <framework>` compiles these libraries inside the runtime
container. Profiling configs consumed by `scripts/profile.sh` choose the hook
library and output paths for each experiment run.

The Dockerfiles and compose services intentionally do not define an entrypoint.
They default to bash. `scripts/build.sh`, `scripts/run.sh`, and
`scripts/profile.sh` provide the command for each flow.

For SGLang on Ascend, the environment image prepares the CANN/PyTorch/torch_npu/
triton-ascend stack and installs the matching `sgl-kernel-npu` release wheels.
The runtime image only copies and installs the editable SGLang fork through
`scripts/frameworks/sglang/install_from_source.sh`, so framework edits only
require rebuilding the runtime layer.

For ktransformers on Ascend, the environment image uses an Ubuntu CANN base and
builds `torch_npu` from source. The runtime image copies and installs the
ktransformers source through `scripts/frameworks/ktransformers/install_from_source.sh`.
That script targets the
`third_party/ktransformers/archive` runtime because the current Ascend NPU
tutorial still uses the legacy `balance_serve` server path. Runtime scripts do
not install `torch_npu` from PyPI or external wheels; they only verify the
image-built package before installing ktransformers.

The intended flow is:

1. `scripts/build.sh <framework>` builds images and `libhook.so`.
2. `scripts/build.sh <framework> --skip-env` rebuilds only the source-install runtime image plus hook.
3. `scripts/build.sh <framework> --hook-only` rebuilds only `libhook.so`.
4. `scripts/profile.sh <config.json>` runs a configured profiling experiment.
5. `scripts/run.sh <framework> -- bash` opens an interactive shell when needed.

Profile JSON files support both single-run and suite shapes. In the suite shape,
top-level settings act as defaults and each object in `experiments` overrides
only the fields that change. The runner executes the experiments sequentially
and writes each trace result into a numbered subdirectory under the suite run
directory.

An experiment can remove an inherited setting with `$unset`, for example:

```json
{
  "name": "without-streams",
  "$unset": ["env.STREAMS_PER_DEVICE", "profile.profile_stages"]
}
```

Adding another inference framework should follow the same shape: add a Docker
image directory, a runtime install script, an optional hook target/profile, and one
service in `docker/compose/inference.yml`.

## Host Analysis Zone

Use the host for trace modeling and optimization:

- build DAGs with `build/bin/trace_graph`
- run what-if scaling through `trace_graph --scale`
- stage raw traces under `data/traces/raw`
- write merged traces to `data/traces/merged`
- write generated DAGs and what-if outputs to `data/traces/dag`

Trace merge scripts can stay host-side and common unless a framework needs
different timestamp matching or profiler output handling.

This keeps the heavyweight inference runtime separate from the graph modeling
loop, while still allowing the profiling container to emit traces into host
mounted directories.
