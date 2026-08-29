#!/usr/bin/env bash
# 从宿主机进入 modeling 容器，执行唯一的建模业务入口。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/lib/common.sh"

ROOT_DIR="$(repo_root)"
CONTAINER_ROOT="/workspace/trace-sim"
cd "$ROOT_DIR"

usage() {
    cat >&2 <<'EOF'
usage:
  scripts/model.sh build-dag (--profile-manifest <manifest> --output-dir <dir> | --config <runner_config.json>)
  scripts/model.sh calibrate-hicache <physical|runtime-dma> [options]
  scripts/model.sh build-hicache-model --calibration-report <report> --base-observations <observations> --output-dir <dir>
  scripts/model.sh predict-hicache [workflow options]
  scripts/model.sh evaluate-hicache [matrix options]

All modeling actions run inside one modeling container. predict-hicache selects
the Direct I/O/control prediction and executes its model cells in that same
container; it does not start one nested container per cell.

examples:
  scripts/model.sh build-dag --profile-manifest <profile_manifest.json> --output-dir <dag-output>
  scripts/model.sh calibrate-hicache physical --help
  scripts/model.sh calibrate-hicache runtime-dma --help
  scripts/model.sh build-hicache-model --help
  scripts/model.sh predict-hicache --source-manifest <manifest> --target-config <config> --hicache-io-model <model>
  scripts/model.sh evaluate-hicache --profile-run-dir <suite> --base-io-model <base>=<model>
EOF
}

if [ $# -eq 0 ]; then
    usage
    exit 2
fi

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
    usage
    exit 0
fi

action=$1
shift
container_command=()
action_args=()

case "$action" in
    build-dag)
        container_command=(python3 scripts/internal/entrypoints/model.py)
        action_args=("$@")
        ;;
    calibrate-hicache)
        calibration_kind=${1:-}
        if [ -z "$calibration_kind" ]; then
            usage
            exit 2
        fi
        shift
        case "$calibration_kind" in
            physical)
                container_command=(python3 -m markov_internal.modeling_workflow.io_calibration)
                action_args=("$@")
                ;;
            runtime-dma)
                container_command=(python3 -m markov_internal.modeling_workflow.runtime_dma_calibration)
                action_args=("$@")
                ;;
            *)
                echo "unknown HiCache calibration kind: $calibration_kind" >&2
                usage
                exit 2
                ;;
        esac
        ;;
    build-hicache-model)
        container_command=(python3 -m markov_internal.modeling_workflow.io_model_builder)
        action_args=("$@")
        ;;
    predict-hicache)
        container_command=(python3 scripts/internal/entrypoints/modeling_workflow.py)
        action_args=("$@")
        ;;
    evaluate-hicache)
        container_command=(python3 scripts/internal/entrypoints/modeling_workflow.py)
        action_args=(--evaluation "$@")
        ;;
    *)
        echo "unknown modeling action: $action" >&2
        usage
        exit 2
        ;;
esac

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
for arg in "${action_args[@]}"; do
    container_args+=("$(container_arg "$arg")")
done

docker compose -f "$(compose_file)" run --rm \
    -e TRACE_SIM_MODELING_CONTAINER=1 \
    modeling \
    env PYTHONPATH=scripts/internal \
    "${container_command[@]}" "${container_args[@]}"
