#!/usr/bin/env bash
set -eo pipefail
source /usr/local/Ascend/ascend-toolkit/set_env.sh
set -u

# Offline analysis needs driver symbols, but must not discover or initialize physical devices.
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:+${LD_LIBRARY_PATH}:}/usr/local/Ascend/ascend-toolkit/latest/aarch64-linux/devlib"
exec python3 -m markov_collection export "$@"
