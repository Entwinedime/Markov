# Project Structure

The current structure follows the migrated hook framework and trace graph work,
while leaving room for ktransformers and sglang specific adapters.

## Profiling

- `third_party/sglang/`: editable sglang fork submodule used by the runtime container.
- `third_party/ktransformers/`: editable ktransformers fork submodule used by the runtime container.
- `src/cpp/hook_framework/`: C++ LD_PRELOAD hook library.
- `src/cpp/hook_framework/targets/`: framework/runtime-specific hook wrappers.
- `scripts/build.sh`: builds framework images and matching hook libraries.
- `scripts/run.sh`: runs ad-hoc commands in framework runtime containers.
- `scripts/profile.sh`: runs JSON-configured profiling experiments.
- `scripts/libexec/profile_runner.py`: container-side profile runner for server, workload, hook, and SGLang profile API orchestration.
- `scripts/frameworks/sglang/`: SGLang install and profiling internals.
- `scripts/frameworks/ktransformers/`: ktransformers install and profiling internals.
- `scripts/hooks/`: container-side hook build helper.
- `scripts/trace/common/`: shared trace merge implementation.
- `scripts/trace/sglang/` and `scripts/trace/ktransformers/`: framework-specific trace wrappers or future overrides.

Profiling is the containerized stage. SGLang and ktransformers use separate
Docker images, profile scripts, hook build directories, and `libhook.so`
outputs, all wired through the single `docker/compose/inference.yml` file.

## Modeling

- `src/cpp/trace_graph/`: Chrome Trace parser, leaf extraction, dependency DAG, and simulator.
- `data/profile_runs/`: generated container-side profile run outputs.
- `data/traces/raw/`: host-side raw trace staging area.
- `data/traces/merged/`: merged Chrome Trace files used as DAG input.
- `data/traces/dag/`: generated DAG/simulation timeline JSON.

## Optimization

- `src/cpp/trace_graph` currently exposes what-if scaling through the `trace_graph` CLI.
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
