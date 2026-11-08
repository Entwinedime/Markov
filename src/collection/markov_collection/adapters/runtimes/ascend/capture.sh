#!/usr/bin/env bash
set -eo pipefail
source /usr/local/Ascend/ascend-toolkit/set_env.sh
set -u

# Native capture resolves real driver libraries before the toolkit's analysis-only interfaces.
export LD_LIBRARY_PATH="/usr/local/Ascend/driver/lib64/common:/usr/local/Ascend/driver/lib64/driver:${LD_LIBRARY_PATH:-}"
exec python3 -m markov_collection capture "$@"
