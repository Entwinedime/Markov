#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

RUN_ROOT="${ROOT_DIR}/auto-profile-logs"
RUN_ID="$(date +"%Y%m%d_%H%M%S")"
RUN_DIR="${RUN_ROOT}/${RUN_ID}"

PORT=30000
SERVER_READY_TIMEOUT=3600
STOP_WAIT_SECONDS=10

export SGLANG_SET_CPU_AFFINITY=1
export PYTORCH_NPU_ALLOC_CONF=expandable_segments:True
export STREAMS_PER_DEVICE=32
export HCCL_BUFFSIZE=1536
mkdir -p "$RUN_DIR"

CONFIG_BACKUP_DIR="${RUN_DIR}/_config_backup"

# Optional: set to empty to keep original layers.
FORCE_V2_LAYERS="${FORCE_V2_LAYERS:-}"
FORCE_R1_LAYERS="${FORCE_R1_LAYERS:-10}"
FORCE_QWEN_LAYERS="${FORCE_QWEN_LAYERS:-20}"

V2_MODEL_PATH="/root/models/DeepSeek-V2-Lite-Chat"
R1_MODEL_PATH="/root/models/DeepSeek-R1-0528-w4a8"
QWEN_MODEL_PATH="/root/models/Qwen3-235B-A22B-Instruct-2507"

CURRENT_SERVER_PID=""
CURRENT_MODEL_PATH=""
CURRENT_NEEDS_RESTORE=0

log() {
    echo "[$(date +"%Y-%m-%d %H:%M:%S")] $*"
}

config_path() {
    echo "${1%/}/config.json"
}

backup_config() {
    local model_path=$1
    local config
    config="$(config_path "$model_path")"
    if [ ! -f "$config" ]; then
        log "Missing config.json: $config"
        return 1
    fi
    local backup="${CONFIG_BACKUP_DIR}/$(basename "$model_path")/config.json"
    if [ ! -f "$backup" ]; then
        mkdir -p "$(dirname "$backup")"
        cp "$config" "$backup"
    fi
}

restore_config() {
    local model_path=$1
    local config
    config="$(config_path "$model_path")"
    local backup="${CONFIG_BACKUP_DIR}/$(basename "$model_path")/config.json"
    if [ -f "$backup" ]; then
        cp "$backup" "$config"
    fi
}

set_num_hidden_layers() {
    local model_path=$1
    local layers=$2
    if ! [[ "$layers" =~ ^[0-9]+$ ]]; then
        log "Invalid num_hidden_layers: $layers"
        return 1
    fi
    local config
    config="$(config_path "$model_path")"
    python3 - "$config" "$layers" <<'PY'
import json
import sys

path = sys.argv[1]
layers = int(sys.argv[2])

with open(path, "r", encoding="utf-8") as f:
    data = json.load(f)
data["num_hidden_layers"] = layers
with open(path, "w", encoding="utf-8") as f:
    json.dump(data, f, indent=2)
    f.write("\n")
PY
}

wait_for_server() {
    local port=$1
    local pid=$2
    local timeout=$3
    local start
    start=$(date +%s)
    while true; do
        if curl -s "http://127.0.0.1:${port}/get_model_info" >/dev/null 2>&1; then
            return 0
        fi
        if ! kill -0 "$pid" >/dev/null 2>&1; then
            return 1
        fi
        if (($(date +%s) - start > timeout)); then
            return 1
        fi
        sleep 5
    done
}

stop_server() {
    local pid=$1
    if kill -0 "$pid" >/dev/null 2>&1; then
        kill "$pid" >/dev/null 2>&1 || true
        wait "$pid" >/dev/null 2>&1 || true
    fi
}

cleanup_current() {
    if [ -n "$CURRENT_SERVER_PID" ]; then
        stop_server "$CURRENT_SERVER_PID"
        CURRENT_SERVER_PID=""
    fi
    if [ "$CURRENT_NEEDS_RESTORE" -eq 1 ] && [ -n "$CURRENT_MODEL_PATH" ]; then
        restore_config "$CURRENT_MODEL_PATH"
        CURRENT_NEEDS_RESTORE=0
        CURRENT_MODEL_PATH=""
    fi
}

on_interrupt() {
    log "Interrupted. Cleaning up and exiting."
    cleanup_current
    exit 130
}

trap 'cleanup_current' EXIT
trap 'on_interrupt' INT TERM

run_bench() {
    local bench_dir=$1
    local torch_dir=$2

    mkdir -p "$bench_dir" "$torch_dir"
    SGLANG_TORCH_PROFILER_DIR="$torch_dir" python3 -m sglang.bench_serving \
        --backend sglang \
        --num-prompts 1 \
        --dataset-name random-ids \
        --random-input 64 \
        --random-output 32 \
        --random-range-ratio 0.5 \
        --output-file "${bench_dir}/bench.jsonl" \
        --profile \
        >"${bench_dir}/bench.log" 2>&1
}

run_experiment() {
    local exp_name=$1
    local model_path=$2
    local num_layers=$3
    shift 3
    local -a server_cmd=("$@")

    CURRENT_MODEL_PATH="$model_path"
    CURRENT_NEEDS_RESTORE=0
    CURRENT_SERVER_PID=""

    if [ -n "$num_layers" ]; then
        if ! backup_config "$model_path"; then
            return 1
        fi
        if ! set_num_hidden_layers "$model_path" "$num_layers"; then
            restore_config "$model_path"
            return 1
        fi
        CURRENT_NEEDS_RESTORE=1
    fi

    local exp_dir="${RUN_DIR}/${exp_name}"
    local serve_dir="${exp_dir}/serve"
    local bench_dir="${exp_dir}/bench"
    local trace_dir="${exp_dir}/trace"
    local torch_dir="${trace_dir}/torch"

    mkdir -p "$serve_dir" "$bench_dir" "$trace_dir" "$torch_dir"

    printf '%q ' "${server_cmd[@]}" >"${exp_dir}/server_cmd.txt"
    echo >>"${exp_dir}/server_cmd.txt"
    printf '%q ' python3 -m sglang.bench_serving \
        --backend sglang \
        --num-prompts 1 \
        --dataset-name random-ids \
        --random-input 64 \
        --random-output 32 \
        --random-range-ratio 0.5 \
        --output-file "${bench_dir}/bench.jsonl" \
        --profile >"${exp_dir}/bench_cmd.txt"
    echo >>"${exp_dir}/bench_cmd.txt"

    log "Starting server: ${exp_name}"
    LD_PRELOAD="${ROOT_DIR}/CppHookFrameWork/libhook.so" \
        SGLANG_TORCH_PROFILER_DIR="$torch_dir" \
        HOOK_TRACE_OUTPUT="${trace_dir}/cpu_trace" \
        "${server_cmd[@]}" >"${serve_dir}/serve.log" 2>&1 &
    CURRENT_SERVER_PID=$!

    if ! wait_for_server "$PORT" "$CURRENT_SERVER_PID" "$SERVER_READY_TIMEOUT"; then
        log "Server failed to start: ${exp_name}"
        cleanup_current
        return 1
    fi

    log "Running bench: ${exp_name}"
    run_bench "$bench_dir" "$torch_dir"

    log "Stopping server: ${exp_name}"
    cleanup_current

    sleep "$STOP_WAIT_SECONDS"
}

V2_TP_LIST=(1 2 4 8)
V2_OFFLOADV1_TP_LIST=(1)
V2_OFFLOADV1_GB_LIST=(16 20)
V2_OFFLOADV2_TP_LIST=(1)
V2_OFFLOADV2_CONFIGS=(
    "1,1,2"
    "1,1,4"
    "1,2,4"
    "2,2,4"
)

V2_BASE=(
    python3 -m sglang.launch_server
    --model-path "$V2_MODEL_PATH"
    --trust-remote-code
    --attention-backend ascend
    --device npu
    --watchdog-timeout 9000
    --mem-fraction-static 0.6
)

R1_TP_LIST=(1 2 4 8)

R1_BASE=(
    python3 -m sglang.launch_server
    --model-path "$R1_MODEL_PATH"
    --trust-remote-code
    --attention-backend ascend
    --device npu
    --quantization modelslim
    --watchdog-timeout 9000
    --mem-fraction-static 0.8
    --dtype bfloat16
)

QWEN3_TP_LIST=(2 4 8)

QWEN3_BASE=(
    python3 -m sglang.launch_server
    --model-path "$QWEN_MODEL_PATH"
    --trust-remote-code
    --attention-backend ascend
    --device npu
    --watchdog-timeout 9000
    --mem-fraction-static 0.8
)

log "Run dir: ${RUN_DIR}"

# for tp in "${V2_TP_LIST[@]}"; do
#     run_experiment "deepseek-v2-lite-chat_tp${tp}" "$V2_MODEL_PATH" "$FORCE_V2_LAYERS" \
#         "${V2_BASE[@]}" \
#         --tp-size "$tp"
# done

for tp in "${V2_OFFLOADV1_TP_LIST[@]}"; do
    for gb in "${V2_OFFLOADV1_GB_LIST[@]}"; do
        run_experiment "deepseek-v2-lite-chat_tp${tp}_offloadv1_cpu${gb}g" "$V2_MODEL_PATH" "$FORCE_V2_LAYERS" \
            "${V2_BASE[@]}" \
            --tp-size "$tp" \
            --disable-cuda-graph \
            --cpu-offload-gb "$gb"
    done
done

# for tp in "${V2_OFFLOADV2_TP_LIST[@]}"; do
#     for cfg in "${V2_OFFLOADV2_CONFIGS[@]}"; do
#         IFS=',' read -r prefetch num_in_group group_size <<<"$cfg"
#         run_experiment "deepseek-v2-lite-chat_tp${tp}_offloadv2_p${prefetch}_n${num_in_group}_g${group_size}" "$V2_MODEL_PATH" "$FORCE_V2_LAYERS" \
#             "${V2_BASE[@]}" \
#             --tp-size "$tp" \
#             --disable-cuda-graph \
#             --offload-prefetch-step "$prefetch" \
#             --offload-num-in-group "$num_in_group" \
#             --offload-group-size "$group_size" \
#             --offload-mode cpu
#     done
# done

# for tp in "${R1_TP_LIST[@]}"; do
#     run_experiment "deepseek-r1_tp${tp}" "$R1_MODEL_PATH" "$FORCE_R1_LAYERS" \
#         "${R1_BASE[@]}" \
#         --tp-size "$tp"
# done

# for tp in "${QWEN3_TP_LIST[@]}"; do
#     run_experiment "qwen3-235b-a22b_tp${tp}" "$QWEN_MODEL_PATH" "$FORCE_QWEN_LAYERS" \
#         "${QWEN3_BASE[@]}" \
#         --tp-size "$tp"
# done
