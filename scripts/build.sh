#!/usr/bin/env bash
# 构建指定 Docker 环境镜像，或构建框架 runtime 镜像及 LD_PRELOAD hook。
#
# 外层脚本只负责编排 Docker build 与容器内 hook build；具体源码安装逻辑保持在
# `scripts/internal/frameworks/*/install_from_source.sh` 中。modeling 环境是干净的
# Ubuntu 24.04 C++23 工具链，不构建 Ascend/CANN runtime 或 hook。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/lib/common.sh"

ROOT_DIR="$(repo_root)"
cd "$ROOT_DIR"

# 打印构建入口的命令行用法。
usage() {
    cat >&2 <<'EOF'
usage: scripts/build.sh <sglang|ktransformers|modeling> [--skip-env] [--image-only] [--hook-only]

Builds the selected environment.

Options:
  --skip-env    Rebuild only the source-install runtime image layer.
  --image-only  Build images, then stop before building libhook.so.
  --hook-only   Skip Docker image builds and build only libhook.so in the runtime image.

Notes:
  modeling builds only markov-trace-sim-modeling:ubuntu24.04.
EOF
}

TARGET="${1:-}"
if [ -z "$TARGET" ] || [ "$TARGET" = "-h" ] || [ "$TARGET" = "--help" ]; then
    usage
    if [ -z "$TARGET" ]; then
        exit 2
    fi
    exit 0
fi
shift
SERVICE="$(container_service "$TARGET")"

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

if [ "$TARGET" = "modeling" ]; then
    if [ "$SKIP_ENV" = "1" ] || [ "$HOOK_ONLY" = "1" ]; then
        echo "--skip-env and --hook-only do not apply to modeling" >&2
        usage
        exit 2
    fi
    docker compose -f "$(compose_file)" build modeling
    exit 0
fi

# 两个推理框架采用相同镜像布局；各自的依赖和安装步骤留在 Dockerfile。
if [ "$HOOK_ONLY" != "1" ]; then
    if [ "$SKIP_ENV" != "1" ]; then
        docker build \
            -f "docker/images/base/$TARGET/Dockerfile" \
            -t "markov-trace-sim-$TARGET-env:ubuntu22.04" \
            "docker/images/base/$TARGET"
    fi
    docker compose -f "$(compose_file)" build "$SERVICE"
fi

if [ "$IMAGE_ONLY" != "1" ]; then
    "$SCRIPT_DIR/run.sh" "$TARGET" -- bash -lc 'exec scripts/internal/hooks/build.sh "$@"' bash "$TARGET"
fi
