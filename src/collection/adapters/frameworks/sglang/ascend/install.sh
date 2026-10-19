#!/usr/bin/env bash
# Packaging changes and build outputs belong only to the image-local copy.
set -euo pipefail

if (($# != 1)); then
    printf 'usage: %s <trace-sim-root>\n' "$0" >&2
    exit 2
fi

MARKOV_SGLANG_SOURCE=/opt/markov/sglang
mkdir -p "$MARKOV_SGLANG_SOURCE"
cp -a "$1/third_party/sglang/." "$MARKOV_SGLANG_SOURCE/"

set +u
source /usr/local/Ascend/ascend-toolkit/set_env.sh
set -u
export LD_LIBRARY_PATH="/usr/local/Ascend/driver/lib64/common:/usr/local/Ascend/driver/lib64/driver:${LD_LIBRARY_PATH:-}"

cd "$MARKOV_SGLANG_SOURCE/python"
cp pyproject_npu.toml pyproject.toml
python3 -m pip install --no-cache-dir -v -e '.[all_npu]'
