#!/usr/bin/env bash
# This directory is an external example, not a formal Markov command.
set -euo pipefail

EXAMPLE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

exec python3 "${EXAMPLE_DIR}/run.py" "$@"
