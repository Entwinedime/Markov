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
