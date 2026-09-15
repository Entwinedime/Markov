#!/usr/bin/env bash
# 宿主机上的建模入口；设备校准使用框架容器，其余计算使用 modeling。
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
  scripts/model.sh build-hicache-model --group <group_request.json> [--output-dir <dir>]
  scripts/model.sh prepare-hicache --group <group_request.json> [--dry-run] [--calibration-only] [--refresh-observations] [--model-run-jobs <n>] [--diagnostics off|full]
  scripts/model.sh predict-hicache [workflow options]
  scripts/model.sh evaluate-hicache --prediction-dir <completed-output> --profile-run-dir <target-suite> --output-dir <score-output> [--oracle-cost-replay]

prepare-hicache coordinates containers from the host. Its dry-run writes fixed-input
and readiness plans without starting an inference server. With sufficient admitted
group observations it builds one model and predicts the group. --calibration-only
stops before model build/prediction. Missing base runs require a separate
base_capture_budget; without it only a plan is returned.
Missing physical data requires a separately budgeted physical_capture request;
without it only a measurement plan is returned. That request must explicitly
declare target-independent physical_capture.page_token_sizes.

Physical/runtime-DMA calibration runs in the SGLang device environment.
Other modeling actions run inside one modeling container. predict-hicache selects
the HiCache I/O/control prediction and executes its model cells in that same
container; it does not start one nested container per cell.

examples:
  scripts/model.sh build-dag --profile-manifest <profile_manifest.json> --output-dir <dag-output>
  scripts/model.sh calibrate-hicache physical --help
  scripts/model.sh calibrate-hicache runtime-dma --help
  scripts/model.sh build-hicache-model --help
  scripts/model.sh predict-hicache --source-manifest <manifest> --target-config <config> --hicache-io-model <model>
  scripts/model.sh evaluate-hicache --prediction-dir <predictions> --profile-run-dir <suite> --output-dir <scores>
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
model_environment=modeling

case "$action" in
    prepare-hicache)
        exec env PYTHONPATH=scripts/internal python3 -m markov_internal.modeling_workflow.prepare "$@"
        ;;
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
                model_environment=sglang
                container_command=(python3 -m markov_internal.modeling_workflow.io_calibration)
                action_args=("$@")
                ;;
            runtime-dma)
                model_environment=sglang
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
        container_command=(python3 -m markov_internal.modeling_workflow.evaluation.existing)
        action_args=("$@")
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

model_python_path=scripts/internal
if [ "$model_environment" = sglang ]; then
    model_python_path+=:third_party/sglang/python
fi
exec "$SCRIPT_DIR/run.sh" "$model_environment" -- \
    env PYTHONPATH="$model_python_path" \
    "${container_command[@]}" "${container_args[@]}"
