#!/usr/bin/env bash
set -euo pipefail

MARKOV_PROFILER_INSTALL=$(cd "$(dirname "$0")" && pwd)
MARKOV_TORCH_NPU_ROOT=$(python3 -c "import importlib.util; print(importlib.util.find_spec('torch_npu').submodule_search_locations[0])")
MARKOV_TORCH_NPU_REVISION=$(python3 -c "import runpy, sys; print(runpy.run_path(sys.argv[1])['git_version'])" "$MARKOV_TORCH_NPU_ROOT/version.py")
MARKOV_PROFILER_SOURCE=$(mktemp -d)
trap 'rm -r "$MARKOV_PROFILER_SOURCE"' EXIT

# Publish the running flag before pthread_create, so a new dumper cannot exit at startup.
curl --fail --location --max-time 120 \
    "https://codeload.github.com/Ascend/pytorch/tar.gz/$MARKOV_TORCH_NPU_REVISION" \
    | tar --extract --gzip --directory "$MARKOV_PROFILER_SOURCE" --strip-components=1 \
        --wildcards '*/torch_npu/csrc/toolkit/profiler/*'
patch --batch --fuzz=0 --directory "$MARKOV_PROFILER_SOURCE" --strip=1 < "$MARKOV_PROFILER_INSTALL/start.patch"

# Building this CPU-only library does not load or initialize the device backend.
TORCH_DEVICE_BACKEND_AUTOLOAD=0 cmake -S "$MARKOV_PROFILER_INSTALL" -B "$MARKOV_PROFILER_SOURCE/build" \
    -DCMAKE_BUILD_TYPE=Release -DMARKOV_TORCH_NPU_SOURCE_DIR="$MARKOV_PROFILER_SOURCE"
cmake --build "$MARKOV_PROFILER_SOURCE/build" --target npu_profiler --parallel 2
install -m 755 "$MARKOV_PROFILER_SOURCE/build/profiler/libnpu_profiler.so" \
    "$MARKOV_TORCH_NPU_ROOT/lib/libnpu_profiler.so"
