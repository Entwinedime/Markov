#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/lib/common.sh"

ROOT_DIR="$(repo_root)"
cd "$ROOT_DIR"

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

build_runtime_image() {
    local service
    service="$(profile_service "$FRAMEWORK")"
    docker compose -f "$(compose_file)" build "$service"
}

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
