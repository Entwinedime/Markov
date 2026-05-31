#!/usr/bin/env bash
set -euo pipefail

if [ $# -lt 1 ]; then
    echo "usage: $0 <sglang|ktransformers|ascendcl|template>" >&2
    exit 2
fi

HOOK_PROFILE="$1"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/docker/${HOOK_PROFILE}"

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
    -DHOOK_ENABLE_PAPI=OFF \
    -DHOOK_PROFILE="$HOOK_PROFILE"

cmake --build "$BUILD_DIR" --target hook -j"$(nproc)"

echo "${BUILD_DIR}/lib/libhook.so"
