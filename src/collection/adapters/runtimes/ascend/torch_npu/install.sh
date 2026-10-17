#!/usr/bin/env bash
# Rebuild the pinned wheel's native libraries with allocator and queue probes.
set -euo pipefail

MARKOV_ADAPTER_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
TORCH_NPU_COMMIT=94f8a8e6b523d7ba553e1b80d5b5248478391526
OP_PLUGIN_COMMIT=dedc316708372c9a8bfde4527abd2a94b74840f5
FMT_COMMIT=123913715afeb8a437e6388b4473fcc4753e1c9a
JSON_COMMIT=87cda1d6646592ac5866dc703c8e1839046a6806

# The Python extension remains from this exact wheel. A different native ABI
# requires updating the source and deployment together, not trying a fallback.
TORCH_NPU_PACKAGE=$(python3 - "$TORCH_NPU_COMMIT" <<'PY'
import importlib.metadata
import runpy
import sys

distribution = importlib.metadata.distribution("torch-npu")
package = distribution.locate_file("torch_npu")
version = runpy.run_path(str(package / "version.py"))
if version["git_version"] != sys.argv[1] or importlib.metadata.version("torch") != "2.10.0+cpu":
    raise RuntimeError("workspace probe requires the pinned torch_npu wheel and Torch 2.10.0+cpu")
print(package)
PY
)

BUILD_ROOT=$(mktemp -d /tmp/markov-workspace-build.XXXXXX)
trap 'rm -rf "$BUILD_ROOT"' EXIT

fetch_source() {
    local repository="$1" commit="$2" destination="$3"
    mkdir -p "$destination"
    curl --fail --location --max-time 180 \
        "https://codeload.github.com/${repository}/tar.gz/${commit}" \
        | tar -xz --strip-components=1 -C "$destination"
}

fetch_source Ascend/pytorch "$TORCH_NPU_COMMIT" "$BUILD_ROOT"
fetch_source Ascend/op-plugin "$OP_PLUGIN_COMMIT" "$BUILD_ROOT/third_party/op-plugin"
fetch_source fmtlib/fmt "$FMT_COMMIT" "$BUILD_ROOT/third_party/fmt"
fetch_source nlohmann/json "$JSON_COMMIT" "$BUILD_ROOT/third_party/nlohmann"

patch --batch --forward -d "$BUILD_ROOT" -p1 < "$MARKOV_ADAPTER_ROOT/workspace.patch"
patch --batch --forward -d "$BUILD_ROOT" -p1 < "$MARKOV_ADAPTER_ROOT/queue.patch"
patch --batch --forward -d "$BUILD_ROOT" -p1 < "$MARKOV_ADAPTER_ROOT/record_scope.patch"
cp "$MARKOV_ADAPTER_ROOT/queue_observation.hpp" \
    "$BUILD_ROOT/torch_npu/csrc/core/npu/markov_queue_observation.hpp"
cp "$MARKOV_ADAPTER_ROOT/workspace_observation.hpp" \
    "$BUILD_ROOT/torch_npu/csrc/core/npu/markov_workspace_observation.hpp"

set +u
source /usr/local/Ascend/ascend-toolkit/set_env.sh
set -u

(
    cd "$BUILD_ROOT"
    BUILD_WITHOUT_SHA=1 DISABLE_INSTALL_TORCHAIR=TRUE DISABLE_RPC_FRAMEWORK=TRUE \
        MAX_JOBS="${MAX_JOBS:-16}" python3 setup.py build_clib
)

# Regular files are required by torch_npu's loader. No source checkout or
# temporary build path is needed when a capture worker imports the package.
for library in libtorch_npu.so libop_plugin_atb.so libnpu_profiler.so; do
    install -m 755 "$BUILD_ROOT/build/packages/torch_npu/lib/$library" "$TORCH_NPU_PACKAGE/lib/$library"
done
install -m 644 "$BUILD_ROOT/torch_npu/profiler/analysis/prof_parse/_fwk_file_parser.py" \
    "$TORCH_NPU_PACKAGE/profiler/analysis/prof_parse/_fwk_file_parser.py"
install -m 644 "$BUILD_ROOT/torch_npu/profiler/analysis/prof_bean/_torch_op_bean.py" \
    "$TORCH_NPU_PACKAGE/profiler/analysis/prof_bean/_torch_op_bean.py"

python3 - "$TORCH_NPU_PACKAGE" "$TORCH_NPU_COMMIT" "$OP_PLUGIN_COMMIT" "$FMT_COMMIT" "$JSON_COMMIT" <<'PY'
import json
import sys
from pathlib import Path

provenance = {
    "torch_npu_source_commit": sys.argv[2],
    "dependencies": {"op-plugin": sys.argv[3], "fmt": sys.argv[4], "nlohmann": sys.argv[5]},
    "source_patch": "src/collection/adapters/runtimes/ascend/torch_npu/workspace.patch",
    "queue_patch": "src/collection/adapters/runtimes/ascend/torch_npu/queue.patch",
    "queue_probe_header": "src/collection/adapters/runtimes/ascend/torch_npu/queue_observation.hpp",
    "record_scope_patch": "src/collection/adapters/runtimes/ascend/torch_npu/record_scope.patch",
    "probe_header": "src/collection/adapters/runtimes/ascend/torch_npu/workspace_observation.hpp",
    "libraries": ["libtorch_npu.so", "libop_plugin_atb.so", "libnpu_profiler.so"],
    "profiler_reader": "profiler/analysis/prof_parse/_fwk_file_parser.py",
    "scope": "shared workspace allocation and cached release; queue identities for scoped synchronous transfers; uncached release is not observed",
}
(Path(sys.argv[1]) / "markov_workspace_build.json").write_text(json.dumps(provenance, indent=2) + "\n")
PY
