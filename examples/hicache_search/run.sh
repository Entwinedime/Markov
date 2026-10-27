#!/usr/bin/env bash
set -euo pipefail

EXAMPLE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${EXAMPLE_DIR}/../../scripts/lib/common.sh"

cd "$(repo_root)"
export PYTHONPATH="${PWD}/scripts/internal${PYTHONPATH:+:${PYTHONPATH}}"

exec python3 "${EXAMPLE_DIR}/run.py" "$@"
