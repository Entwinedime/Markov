#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ "${1:-}" != modeling ]]; then
    echo 'usage: scripts/run.sh modeling [--] command [arguments...]' >&2
    exit 2
fi
shift

if [[ "${1:-}" == -- ]]; then
    shift
fi

args=(run --rm --network none --pids-limit 256)
args+=(--memory "${MARKOV_MODELING_MEMORY:-4g}" --memory-swap "${MARKOV_MODELING_MEMORY:-4g}")
args+=(--mount "type=bind,source=${ROOT_DIR},target=${ROOT_DIR}" --workdir "$ROOT_DIR")
args+=(-e "PYTHONPATH=${ROOT_DIR}/python")
args+=("${MARKOV_MODELING_IMAGE:-markov-modeling:ubuntu24.04}")

exec docker "${args[@]}" "$@"
