#!/usr/bin/env bash
# Internal build entrypoint for the LD_PRELOAD hook.
#
# Supports both the host-local `ld_preload` profile and framework profiles in
# Docker runtimes. Each profile has an isolated CMake build directory.
set -euo pipefail

if [ $# -lt 1 ]; then
    echo "usage: $0 <ld_preload|sglang|ktransformers|ascendcl|template>" >&2
    exit 2
fi

HOOK_PROFILE="$1"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
SOURCE_DIR="${ROOT_DIR}/src/profiling/ld_preload"

USE_ASCENDCL_TARGETS=ON
case "$HOOK_PROFILE" in
    ld_preload)
        BUILD_DIR="${ROOT_DIR}/build/profiling/ld_preload"
        HOOK_PROFILE=template
        USE_ASCENDCL_TARGETS=OFF
        ;;
    sglang|ktransformers|ascendcl|template)
        BUILD_DIR="${ROOT_DIR}/build/docker/${HOOK_PROFILE}"
        ;;
    *)
        echo "usage: $0 <ld_preload|sglang|ktransformers|ascendcl|template>" >&2
        exit 2
        ;;
esac

cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" \
    -DHOOK_ENABLE_PAPI=OFF \
    -DHOOK_USE_ASCENDCL_TARGETS="$USE_ASCENDCL_TARGETS" \
    -DHOOK_PROFILE="$HOOK_PROFILE" \
    -DCMAKE_LIBRARY_OUTPUT_DIRECTORY="$BUILD_DIR/lib"

cmake --build "$BUILD_DIR" --target hook -j"$(nproc)"

echo "${BUILD_DIR}/lib/libhook.so"
