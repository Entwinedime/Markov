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

# 把可交互运行的环境名称映射为 compose service 名称。
container_service() {
    local environment=${1:-}
    case "$environment" in
        sglang|ktransformers)
            printf '%s-profile\n' "$environment"
            ;;
        modeling)
            printf 'modeling\n'
            ;;
        *)
            echo "unknown environment: ${environment}" >&2
            echo "known environments: sglang, ktransformers, modeling" >&2
            return 2
            ;;
    esac
}
