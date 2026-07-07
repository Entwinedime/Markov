#!/usr/bin/env bash
# 从宿主机进入 modeling 容器，执行 JSON 配置驱动的 modeling run。
#
# 外层入口只负责编排 Docker service、路径投影和容器标记。真实配置解析、
# C++ TraceGraph 调用与 validation 输出都保留在容器内
# `scripts/internal/entrypoints/model.py` 中，避免宿主机 Python/C++ 环境成为隐式依赖。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/lib/common.sh"

ROOT_DIR="$(repo_root)"
CONTAINER_ROOT="/workspace/trace-sim"
cd "$ROOT_DIR"

# 打印外层 modeling 入口的用法。runner 自身的参数通过 `scripts/model.sh --help`
# 转发到容器内 modeling entrypoint 查看。
usage() {
    cat >&2 <<'EOF'
usage: scripts/model.sh <modeling arguments...>

Runs the container-internal modeling runner inside the clean Ubuntu 24.04
modeling Docker service. Use --wrapper-help for this wrapper help.

examples:
  scripts/model.sh --config <workflow_output>/artifacts/runner_configs/target_<config_id>.json \
    --profile-manifest <run_dir>/profile_manifest.json \
    --output-dir <run_dir>/modeling/cache_state \
    --mode cache_state
EOF
}

if [ $# -eq 0 ]; then
    usage
    exit 2
fi

if [ "${1:-}" = "--wrapper-help" ]; then
    usage
    exit 0
fi

# 将仓库内的宿主机绝对路径投影为容器挂载路径。文档和配置仍优先使用
# repo-relative path；这个转换只处理用户手动传入的仓库内绝对路径。
container_arg() {
    local arg=$1
    local name
    local value

    if [ "$arg" = "$ROOT_DIR" ]; then
        printf '%s\n' "$CONTAINER_ROOT"
        return
    fi
    if [[ "$arg" == "$ROOT_DIR/"* ]]; then
        printf '%s/%s\n' "$CONTAINER_ROOT" "${arg#"$ROOT_DIR"/}"
        return
    fi
    if [[ "$arg" == --*=* ]]; then
        name="${arg%%=*}"
        value="${arg#*=}"
        if [ "$value" = "$ROOT_DIR" ]; then
            printf '%s=%s\n' "$name" "$CONTAINER_ROOT"
            return
        fi
        if [[ "$value" == "$ROOT_DIR/"* ]]; then
            printf '%s=%s/%s\n' "$name" "$CONTAINER_ROOT" "${value#"$ROOT_DIR"/}"
            return
        fi
    fi
    printf '%s\n' "$arg"
}

container_args=()
for arg in "$@"; do
    container_args+=("$(container_arg "$arg")")
done

docker compose -f "$(compose_file)" run --rm \
    -e TRACE_SIM_MODELING_CONTAINER=1 \
    modeling \
    python3 scripts/internal/entrypoints/model.py "${container_args[@]}"
