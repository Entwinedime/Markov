#!/usr/bin/env bash
# 宿主机采集入口；参数解析和容器调度共用 Python profiling 工作流。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/lib/common.sh"

cd "$(repo_root)"
exec env PYTHONPATH=scripts/internal python3 -c \
    'from markov_internal.profiling.runner import host_main; host_main()' "$@"
