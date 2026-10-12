#!/usr/bin/env bash
# Compose the image; the adapter owns its source pins, patch and build steps.
set -euo pipefail

exec bash "${1:?usage: $0 <trace-sim-root>}/src/collection/adapters/runtimes/ascend/torch_npu/install.sh"
