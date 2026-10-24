#!/usr/bin/env bash

# Install from an image-local copy; the official checkout remains untouched.
set -euo pipefail

MARKOV_SGLANG_SOURCE=/opt/markov/sglang
mkdir -p "$MARKOV_SGLANG_SOURCE"
cp -a "$1/third_party/sglang/." "$MARKOV_SGLANG_SOURCE/"

# The vendor environment script reads variables that may be unset under nounset.
set +u
source /usr/local/Ascend/ascend-toolkit/set_env.sh
set -u
export LD_LIBRARY_PATH="/usr/local/Ascend/driver/lib64/common:/usr/local/Ascend/driver/lib64/driver:${LD_LIBRARY_PATH:-}"

cd "$MARKOV_SGLANG_SOURCE/python"
cp pyproject_npu.toml pyproject.toml
python3 -m pip install --no-cache-dir -v -e '.[all_npu]'
