#!/usr/bin/env bash
# Image assembly delegates native adaptation to its owning adapter.
set -euo pipefail

exec bash "${1:?usage: $0 <trace-sim-root>}/src/collection/adapters/frameworks/sglang/ascend/install.sh" "$@"
