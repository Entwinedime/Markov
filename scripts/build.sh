#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
configuration="${1:-Release}"
case "$configuration" in
    Release|Debug) ;;
    *)
        echo 'usage: scripts/build.sh [Release|Debug]' >&2
        exit 2
        ;;
esac

build_dir="${ROOT_DIR}/build/modeling/${configuration}"
diagnostics=OFF
if [[ "$configuration" == Debug ]]; then
    diagnostics=ON
fi

"${ROOT_DIR}/scripts/run.sh" modeling -- cmake -S "$ROOT_DIR" -B "$build_dir" \
    -G Ninja "-DCMAKE_BUILD_TYPE=${configuration}" "-DTRACE_GRAPH_DEBUG=${diagnostics}"

exec "${ROOT_DIR}/scripts/run.sh" modeling -- cmake --build "$build_dir" --parallel "${MARKOV_BUILD_JOBS:-2}"
