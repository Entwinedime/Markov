#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export PYTHONPATH="${ROOT_DIR}/python${PYTHONPATH:+:${PYTHONPATH}}"

exec "${MARKOV_PYTHON:-python3}" "${ROOT_DIR}/examples/hicache_search/run.py" "$@"
