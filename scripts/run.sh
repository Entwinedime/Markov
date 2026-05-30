#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/lib.sh"

ROOT_DIR="$(repo_root)"
cd "$ROOT_DIR"

usage() {
    cat >&2 <<'EOF'
usage:
  scripts/run.sh <sglang|ktransformers> [--] [command...]

Runs a one-off command in the selected framework runtime container.
Use scripts/profile.sh for server/workload/profile experiments.

examples:
  scripts/run.sh sglang
  scripts/run.sh sglang -- bash
  scripts/run.sh ktransformers -- python3 -c 'import torch_npu'
EOF
}

framework="${1:-}"
if [ -z "$framework" ] || [ "$framework" = "-h" ] || [ "$framework" = "--help" ]; then
    usage
    if [ -z "$framework" ]; then
        exit 2
    fi
    exit 0
fi
shift

if [ "${1:-}" = "--" ]; then
    shift
fi

service="$(profile_service "$framework")"
if [ $# -eq 0 ]; then
    set -- bash
fi

docker compose -f "$(compose_file)" run --rm "$service" "$@"
