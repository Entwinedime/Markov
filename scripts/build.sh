#!/usr/bin/env bash
# 构建指定推理框架的运行镜像和 LD_PRELOAD hook。
#
# 外层脚本只负责编排 Docker build 与容器内 hook build；具体源码安装逻辑
# 保持在 `scripts/internal/frameworks/*/install_from_source.sh` 中。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/lib/common.sh"

ROOT_DIR="$(repo_root)"
cd "$ROOT_DIR"

# 打印构建入口的命令行用法。
usage() {
    cat >&2 <<'EOF'
usage: scripts/build.sh <sglang|ktransformers> [--skip-env] [--image-only] [--hook-only]

Builds the selected framework runtime image and its libhook.so.

Options:
  --skip-env    Rebuild only the source-install runtime image layer.
  --image-only  Build images, then stop before building libhook.so.
  --hook-only   Skip Docker image builds and build only libhook.so in the runtime image.
EOF
}

FRAMEWORK="${1:-}"
if [ -z "$FRAMEWORK" ] || [ "$FRAMEWORK" = "-h" ] || [ "$FRAMEWORK" = "--help" ]; then
    usage
    if [ -z "$FRAMEWORK" ]; then
        exit 2
    fi
    exit 0
fi
shift
profile_service "$FRAMEWORK" >/dev/null

SKIP_ENV=0
IMAGE_ONLY=0
HOOK_ONLY=0
while [ $# -gt 0 ]; do
    case "$1" in
        --skip-env)
            SKIP_ENV=1
            ;;
        --image-only)
            IMAGE_ONLY=1
            ;;
        --hook-only)
            HOOK_ONLY=1
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "unknown option: $1" >&2
            usage
            exit 2
            ;;
    esac
    shift
done

# 构建框架基础环境镜像。
build_env_image() {
    local framework=$1

    case "$framework" in
        sglang)
            docker build \
                -f docker/images/base/sglang/Dockerfile \
                -t markov-trace-sim-sglang-env:ubuntu22.04 \
                docker/images/base/sglang
            ;;
        ktransformers)
            docker build \
                -f docker/images/base/ktransformers/Dockerfile \
                -t markov-trace-sim-ktransformers-env:ubuntu22.04 \
                docker/images/base/ktransformers
            ;;
    esac
}

# 构建包含源码安装层的 runtime service 镜像。
build_runtime_image() {
    local service
    service="$(profile_service "$FRAMEWORK")"
    docker compose -f "$(compose_file)" build "$service"
}

# 在 runtime 容器内构建对应 profile 的 libhook.so。
build_hook() {
    run_in_container "$FRAMEWORK" bash -lc "set -euo pipefail; scripts/internal/hooks/build.sh '$FRAMEWORK'"
}

if [ "$HOOK_ONLY" != "1" ]; then
    if [ "$SKIP_ENV" != "1" ]; then
        build_env_image "$FRAMEWORK"
    fi
    build_runtime_image
fi

if [ "$IMAGE_ONLY" != "1" ]; then
    build_hook
fi
