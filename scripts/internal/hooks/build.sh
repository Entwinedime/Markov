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

# Remove a CMake build directory only when its configured source changed.
prepare_build_dir() {
    local build_dir="$1"
    local source_dir="$2"
    local cache_file="${build_dir}/CMakeCache.txt"
    if [ ! -f "$cache_file" ]; then
        return
    fi
    # The hook source moved to src/profiling/ld_preload. A cache configured for
    # another source cannot be reconfigured in place, so only this profile's
    # isolated build directory is removed.
    local cached_source
    cached_source="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$cache_file" | tail -1)"
    if [ "$cached_source" != "$source_dir" ]; then
        rm -rf "$build_dir"
    fi
}

if [ "$HOOK_PROFILE" = "ld_preload" ]; then
    BUILD_DIR="${ROOT_DIR}/build/profiling/ld_preload"
    prepare_build_dir "$BUILD_DIR" "$SOURCE_DIR"
    cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" \
        -DHOOK_ENABLE_PAPI=OFF \
        -DHOOK_USE_ASCENDCL_TARGETS=OFF \
        -DHOOK_PROFILE=template \
        -DCMAKE_LIBRARY_OUTPUT_DIRECTORY="$BUILD_DIR/lib"
    cmake --build "$BUILD_DIR" --target hook -j"$(nproc)"
    echo "${BUILD_DIR}/lib/libhook.so"
    exit 0
fi

case "$HOOK_PROFILE" in
    sglang|ktransformers|ascendcl|template)
        ;;
    *)
        echo "usage: $0 <ld_preload|sglang|ktransformers|ascendcl|template>" >&2
        exit 2
        ;;
esac

BUILD_DIR="${ROOT_DIR}/build/docker/${HOOK_PROFILE}"
prepare_build_dir "$BUILD_DIR" "$SOURCE_DIR"
cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" \
    -DHOOK_ENABLE_PAPI=OFF \
    -DHOOK_USE_ASCENDCL_TARGETS=ON \
    -DHOOK_PROFILE="$HOOK_PROFILE" \
    -DCMAKE_LIBRARY_OUTPUT_DIRECTORY="$BUILD_DIR/lib"

cmake --build "$BUILD_DIR" --target hook -j"$(nproc)"

echo "${BUILD_DIR}/lib/libhook.so"
