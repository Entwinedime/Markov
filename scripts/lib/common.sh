#!/usr/bin/env bash
# shell 入口脚本共享的小型路径和容器工具函数。
#
# 这里不执行有副作用的初始化；调用方显式选择 framework 后再进入 Docker。

# 返回当前仓库根目录。
repo_root() {
    cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd
}

# 返回推理 profiling 使用的 compose 文件路径。
compose_file() {
    printf 'docker/compose/inference.yml\n'
}

# 把 framework 名称映射为 compose service 名称。
profile_service() {
    local framework=${1:-}
    case "$framework" in
        sglang|ktransformers)
            printf '%s-profile\n' "$framework"
            ;;
        *)
            echo "unknown framework: ${framework}" >&2
            echo "known frameworks: sglang, ktransformers" >&2
            return 2
            ;;
    esac
}

# 在指定 framework 的 runtime 容器中执行命令。
run_in_container() {
    local framework=$1
    shift

    local service
    service="$(profile_service "$framework")"
    docker compose -f "$(compose_file)" run --rm "$service" "$@"
}
