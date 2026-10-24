#!/usr/bin/env bash
set -euo pipefail

MARKOV_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$MARKOV_ROOT"

exec docker compose -p markov-next-redesign -f docker/compose/inference.yml \
    run --rm --no-deps modeling \
    flock --shared --no-fork build/automatic-graph.lock build/automatic-graph/markov "$@"
