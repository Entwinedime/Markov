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
Main workflow:
  scripts/model.sh prepare-hicache --group <group_request.json> [--dry-run]
  scripts/model.sh build-dag (--profile-manifest <manifest> --output-dir <dir> | --config <runner_config.json>)
  scripts/model.sh evaluate-hicache --prediction-dir <completed-output> --profile-run-dir <target-suite> --output-dir <score-output>

Start with prepare-hicache for one base and a group of targets. It coordinates
base evidence, shared calibration, model construction and prediction. Evaluation
is separate: target measurements are not required to make predictions.
Use --dry-run to inspect the plan without starting an inference server.

Individual stages and diagnostics (normally coordinated by prepare-hicache):
  scripts/model.sh predict-hicache --source-manifest <manifest> --target-config <config> --hicache-io-model <model>
  scripts/model.sh build-hicache-model --group <group_request.json> [--output-dir <dir>]
  scripts/model.sh prepare-cpu-service --light-manifest <manifest> --profile-manifest <manifest> --tp-size <n> --output-dir <dir>
  scripts/model.sh export-hicache-operation-costs --profile-manifest <independent-manifest> --cpu-service-cost <paired-service> --page-size <n> --output-dir <dir>
  scripts/model.sh calibrate-hicache <physical|runtime-dma|eviction-cpu> [options]

Missing base captures need base_capture_budget. Missing physical measurements
need a budgeted physical_capture request with explicit page_token_sizes.
Without the required budget/input, preparation reports the missing work instead
of starting an unbounded capture. --calibration-only stops before prediction.

prepare-hicache coordinates containers from the host. Device calibration uses
SGLang; DAG construction and prediction use modeling. Prediction cells share
one modeling container. Build-dag also supports KTransformers without HiCache.

Run an action with --help for its options. See docs/modeling_development.md for
group inputs, supported cost coverage and the limitations of current predictions.
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
model_environment=modeling
calibration_kind=""

if [ "$action" = calibrate-hicache ]; then
    if [ $# -eq 0 ]; then
        usage
        exit 2
    fi
    calibration_kind=$1
    shift
    model_environment=sglang
fi

case "$action:$calibration_kind" in
    export-hicache-operation-costs:)
        module=modeling_workflow.calibration.operation_costs
        ;;
    prepare-cpu-service:)
        module=modeling_workflow.calibration.prepare_cpu_service
        ;;
    prepare-hicache:)
        exec env PYTHONPATH=scripts/internal python3 -m markov_internal.modeling_workflow.prepare "$@"
        ;;
    build-dag:)
        module=modeling.runner
        ;;
    calibrate-hicache:physical)
        module=modeling_workflow.io_calibration
        ;;
    calibrate-hicache:runtime-dma)
        module=modeling_workflow.runtime_dma_calibration
        ;;
    calibrate-hicache:eviction-cpu)
        module=modeling_workflow.calibration.eviction_cpu
        ;;
    build-hicache-model:)
        module=modeling_workflow.io_model_builder
        ;;
    predict-hicache:)
        module=modeling_workflow.cli
        ;;
    evaluate-hicache:)
        module=modeling_workflow.evaluation.existing
        ;;
    *)
        echo "unknown modeling action: $action${calibration_kind:+/$calibration_kind}" >&2
        usage
        exit 2
        ;;
esac

# 将仓库内的宿主机绝对路径投影为容器挂载路径。文档和配置仍优先使用
# repo-relative path；这个转换只处理用户手动传入的仓库内绝对路径。
container_arg() {
    local value=$1
    local prefix=""

    if [[ "$value" == --*=* ]]; then
        prefix="${value%%=*}="
        value="${value#*=}"
    fi

    if [ "$value" = "$ROOT_DIR" ]; then
        value=$CONTAINER_ROOT
    elif [[ "$value" == "$ROOT_DIR/"* ]]; then
        value="$CONTAINER_ROOT/${value#"$ROOT_DIR"/}"
    fi

    printf '%s%s\n' "$prefix" "$value"
}

container_args=()
for arg in "$@"; do
    container_args+=("$(container_arg "$arg")")
done

model_python_path=scripts/internal
if [ "$model_environment" = sglang ]; then
    model_python_path+=:third_party/sglang/python
fi
exec "$SCRIPT_DIR/run.sh" "$model_environment" -- \
    env PYTHONPATH="$model_python_path" \
    python3 -m "markov_internal.$module" "${container_args[@]}"
