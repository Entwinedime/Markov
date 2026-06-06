#!/usr/bin/env bash
set -euo pipefail

if [ $# -lt 1 ]; then
    echo "usage: $0 <ld_preload|sglang|ktransformers|ascendcl|template>" >&2
    exit 2
fi

HOOK_PROFILE="$1"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
SOURCE_DIR="${ROOT_DIR}/src/profiling/ld_preload"

prepare_build_dir() {
    local build_dir="$1"
    local source_dir="$2"
    local cache_file="${build_dir}/CMakeCache.txt"
    if [ ! -f "$cache_file" ]; then
        return
    fi
    # 旧实现曾从仓库根 CMake 构建 hook。active 实现迁移到 src/profiling/ld_preload
    # 后，残留 cache 会让 CMake 拒绝重新配置，因此这里仅清理本 profile 的构建目录。
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
