#!/usr/bin/env bash

# The image owns this source tree; the official checkout remains untouched.
set -euo pipefail

# The vendor environment script reads variables that may be unset under nounset.
set +u
source /usr/local/Ascend/ascend-toolkit/set_env.sh
set -u
export LD_LIBRARY_PATH="/usr/local/Ascend/driver/lib64/common:/usr/local/Ascend/driver/lib64/driver:${LD_LIBRARY_PATH:-}"

cd "$1/python"
cp pyproject_npu.toml pyproject.toml
python3 -m pip install --no-cache-dir -v -e '.[all_npu]'

# This official revision exports the FA3 entry used by the pinned SGLang backend.
MARKOV_FA_SOURCE=$(mktemp -d)
git -C "$MARKOV_FA_SOURCE" init
git -C "$MARKOV_FA_SOURCE" fetch --depth 1 https://github.com/MinghuasLab/flash-attention-npu.git 4bfa8e7c7bd28be7c1da7387c4b22941da534544
git -C "$MARKOV_FA_SOURCE" checkout --detach FETCH_HEAD
git -C "$MARKOV_FA_SOURCE" submodule update --init --depth 1 csrc/catlass

# The CANN profiling header is shipped outside the default compiler include path.
CPATH="/usr/local/Ascend/ascend-toolkit/latest/pkg_inc/profiling${CPATH:+:$CPATH}" \
    FLASH_ATTENTION_FORCE_BUILD=TRUE FLASH_ATTN_BUILD_VERSION=v3 \
    python3 -m pip install --no-cache-dir --no-deps --no-build-isolation "$MARKOV_FA_SOURCE"
rm -rf "$MARKOV_FA_SOURCE"
