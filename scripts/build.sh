#!/usr/bin/env bash
set -euo pipefail

MARKOV_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$MARKOV_ROOT"

exec docker compose -p markov-next-redesign -f docker/compose/inference.yml \
    run --rm --no-deps modeling bash -lc \
    'mkdir -p build && exec flock build/automatic-graph.lock bash -c "cmake -S . -B build/automatic-graph -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build/automatic-graph --target markov -j2"'
