#!/usr/bin/env bash
# 在指定 Docker 环境中运行一次性命令。
#
# 该入口用于交互排查和环境检查，不承担 profiling manifest、trace 输出或
# modeling validation 编排职责。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/lib/common.sh"

ROOT_DIR="$(repo_root)"
cd "$ROOT_DIR"

# 打印容器命令入口的命令行用法。
usage() {
    cat >&2 <<'EOF'
usage:
  scripts/run.sh <sglang|ktransformers|modeling> [--] [command...]

Runs a one-off command in the selected Docker environment.
Use scripts/profile.sh for server/workload/profile experiments and
scripts/model.sh for configured modeling runs.

examples:
  scripts/run.sh sglang
  scripts/run.sh sglang -- bash
  scripts/run.sh ktransformers -- python3 -c 'import torch_npu'
  scripts/run.sh modeling -- bash -lc 'c++ --version && cmake --version'
EOF
}

environment="${1:-}"
if [ -z "$environment" ] || [ "$environment" = "-h" ] || [ "$environment" = "--help" ]; then
    usage
    if [ -z "$environment" ]; then
        exit 2
    fi
    exit 0
fi
shift

if [ "${1:-}" = "--" ]; then
    shift
fi

service="$(container_service "$environment")"
if [ $# -eq 0 ]; then
    set -- bash
fi

if [ "$service" = "modeling" ]; then
    docker compose -f "$(compose_file)" run --rm \
        -e TRACE_SIM_MODELING_CONTAINER=1 \
        "$service" "$@"
else
    docker compose -f "$(compose_file)" run --rm "$service" "$@"
fi
