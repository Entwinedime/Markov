#!/usr/bin/env bash
set -euo pipefail

MARKOV_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
export PYTHONPATH="$MARKOV_ROOT/src/control${PYTHONPATH:+:$PYTHONPATH}"

exec python3 -m markov_control.workflow "$@"
