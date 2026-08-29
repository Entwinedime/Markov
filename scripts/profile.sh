#!/usr/bin/env bash
# 从宿主机进入框架容器，执行 JSON 配置驱动的 profiling run 或 suite。
#
# 真实 profiling 必须经过该入口，确保 repo 路径、Ascend 环境和容器内
# `scripts/internal/entrypoints/profile.py` 的执行边界一致。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/lib/common.sh"

ROOT_DIR="$(repo_root)"
cd "$ROOT_DIR"

# 打印 profiling 外层入口的命令行用法。
usage() {
    cat >&2 <<'EOF'
usage:
  scripts/profile.sh <config.json> [--dry-run] [--list-experiments]
      [--experiment ID]... [--experiments ID[,ID...]]
      [--input ID]... [--inputs ID[,ID...]]
      [--server ID]... [--servers ID[,ID...]]
      [--channels CHANNEL[,CHANNEL...]]
      [--forced-token-bundle PATH]

Runs a JSON-configured profiling experiment inside the framework container.
The config chooses the framework, server command, workload, hook settings, and
framework-specific profiling controls.

For suite configs, --list-experiments prints expanded experiment ids. Use
--experiment repeatedly or --experiments with a comma-separated list to run a
subset. Use --input/--inputs and --server/--servers for semantic matrix
selection.
Forced-token replay suites require --forced-token-bundle. The bundle must be
an explicit capture-suite artifact under the repository workspace.
EOF
}

config_path="${1:-}"
if [ -z "$config_path" ] || [ "$config_path" = "-h" ] || [ "$config_path" = "--help" ]; then
    usage
    if [ -z "$config_path" ]; then
        exit 2
    fi
    exit 0
fi
shift

dry_run=0
list_experiments=0
selected_experiments=()
selected_inputs=()
selected_servers=()
forced_token_bundle=""
profile_channels=""
while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run)
            dry_run=1
            shift
            ;;
        --list-experiments)
            list_experiments=1
            shift
            ;;
        --experiment)
            if [ $# -lt 2 ]; then
                echo "--experiment requires an experiment id or name" >&2
                usage
                exit 2
            fi
            selected_experiments+=("$2")
            shift 2
            ;;
        --experiments)
            if [ $# -lt 2 ]; then
                echo "--experiments requires a comma-separated experiment list" >&2
                usage
                exit 2
            fi
            selected_experiments+=("$2")
            shift 2
            ;;
        --input)
            if [ $# -lt 2 ]; then
                echo "--input requires an input id" >&2
                usage
                exit 2
            fi
            selected_inputs+=("$2")
            shift 2
            ;;
        --inputs)
            if [ $# -lt 2 ]; then
                echo "--inputs requires a comma-separated input list" >&2
                usage
                exit 2
            fi
            selected_inputs+=("$2")
            shift 2
            ;;
        --server)
            if [ $# -lt 2 ]; then
                echo "--server requires a server id" >&2
                usage
                exit 2
            fi
            selected_servers+=("$2")
            shift 2
            ;;
        --servers)
            if [ $# -lt 2 ]; then
                echo "--servers requires a comma-separated server list" >&2
                usage
                exit 2
            fi
            selected_servers+=("$2")
            shift 2
            ;;
        --forced-token-bundle)
            if [ $# -lt 2 ]; then
                echo "--forced-token-bundle requires a bundle path" >&2
                usage
                exit 2
            fi
            forced_token_bundle="$2"
            shift 2
            ;;
        --channels)
            if [ $# -lt 2 ]; then
                echo "--channels requires a comma-separated channel list" >&2
                usage
                exit 2
            fi
            profile_channels="$2"
            shift 2
            ;;
        *)
            echo "unknown option: $1" >&2
            usage
            exit 2
            ;;
    esac
done

abs_config="$(python3 - "$config_path" <<'PY'
import sys
from pathlib import Path

print(Path(sys.argv[1]).expanduser().resolve())
PY
)"

case "$abs_config" in
    "$ROOT_DIR"/*) ;;
    *)
        echo "profile config must live under $ROOT_DIR: $abs_config" >&2
        exit 2
        ;;
esac

selected_csv=""
if [ ${#selected_experiments[@]} -gt 0 ]; then
    selected_csv="$(python3 - "${selected_experiments[@]}" <<'PY'
import sys

print(",".join(item for item in sys.argv[1:] if item))
PY
)"
fi
selected_inputs_csv=""
if [ ${#selected_inputs[@]} -gt 0 ]; then
    selected_inputs_csv="$(python3 - "${selected_inputs[@]}" <<'PY'
import sys

print(",".join(item for item in sys.argv[1:] if item))
PY
)"
fi
selected_servers_csv=""
if [ ${#selected_servers[@]} -gt 0 ]; then
    selected_servers_csv="$(python3 - "${selected_servers[@]}" <<'PY'
import sys

print(",".join(item for item in sys.argv[1:] if item))
PY
)"
fi
container_forced_token_bundle=""
if [ -n "$forced_token_bundle" ]; then
    abs_forced_token_bundle="$(python3 - "$forced_token_bundle" <<'PY'
import sys
from pathlib import Path

print(Path(sys.argv[1]).expanduser().resolve())
PY
)"
    case "$abs_forced_token_bundle" in
        "$ROOT_DIR"/*) ;;
        *)
            echo "forced token bundle must live under $ROOT_DIR: $abs_forced_token_bundle" >&2
            exit 2
            ;;
    esac
    if [ ! -f "$abs_forced_token_bundle" ]; then
        echo "forced token bundle does not exist: $abs_forced_token_bundle" >&2
        exit 2
    fi
    container_forced_token_bundle="/workspace/trace-sim/${abs_forced_token_bundle#"$ROOT_DIR"/}"
fi

framework="$(python3 - "$abs_config" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as f:
    data = json.load(f)
print(data.get("framework", "sglang"))
PY
)"
service="$(profile_service "$framework")"
container_config="/workspace/trace-sim/${abs_config#"$ROOT_DIR"/}"

case "$framework" in
    sglang)
        ascendcl_so_path="/usr/local/Ascend/ascend-toolkit/latest/lib64/libascendcl.so"
        ;;
    ktransformers)
        ascendcl_so_path="/usr/local/Ascend/ascend-toolkit/latest/lib64/libascendcl.so"
        ;;
    *)
        echo "unknown framework: $framework" >&2
        exit 2
        ;;
esac

container_cmd='
set -euo pipefail
set +u
source /usr/local/Ascend/ascend-toolkit/set_env.sh
set -u
export LD_LIBRARY_PATH="/usr/local/Ascend/driver/lib64/common:/usr/local/Ascend/driver/lib64/driver:${LD_LIBRARY_PATH:-}"
export HOOK_ASCENDCL_SO_PATH="$TRACE_SIM_HOOK_ASCENDCL_SO_PATH"
runner_args=(--config "$TRACE_SIM_PROFILE_CONFIG")
if [ -n "${TRACE_SIM_PROFILE_DRY_RUN:-}" ]; then
    runner_args+=(--dry-run)
fi
if [ -n "${TRACE_SIM_PROFILE_LIST_EXPERIMENTS:-}" ]; then
    runner_args+=(--list-experiments)
fi
if [ -n "${TRACE_SIM_PROFILE_EXPERIMENTS:-}" ]; then
    runner_args+=(--experiments "$TRACE_SIM_PROFILE_EXPERIMENTS")
fi
if [ -n "${TRACE_SIM_PROFILE_INPUTS:-}" ]; then
    runner_args+=(--inputs "$TRACE_SIM_PROFILE_INPUTS")
fi
if [ -n "${TRACE_SIM_PROFILE_SERVERS:-}" ]; then
    runner_args+=(--servers "$TRACE_SIM_PROFILE_SERVERS")
fi
if [ -n "${TRACE_SIM_FORCED_TOKEN_BUNDLE:-}" ]; then
    runner_args+=(--forced-token-bundle "$TRACE_SIM_FORCED_TOKEN_BUNDLE")
fi
if [ -n "${TRACE_SIM_PROFILE_CHANNELS:-}" ]; then
    runner_args+=(--channels "$TRACE_SIM_PROFILE_CHANNELS")
fi
python3 scripts/internal/entrypoints/profile.py "${runner_args[@]}"
'

env_args=(
    -e "TRACE_SIM_PROFILE_CONFIG=${container_config}"
    -e "TRACE_SIM_HOOK_ASCENDCL_SO_PATH=${ascendcl_so_path}"
)
if [ "$dry_run" = "1" ]; then
    env_args+=(-e "TRACE_SIM_PROFILE_DRY_RUN=1")
fi
if [ "$list_experiments" = "1" ]; then
    env_args+=(-e "TRACE_SIM_PROFILE_LIST_EXPERIMENTS=1")
fi
if [ -n "$selected_csv" ]; then
    env_args+=(-e "TRACE_SIM_PROFILE_EXPERIMENTS=${selected_csv}")
fi
if [ -n "$selected_inputs_csv" ]; then
    env_args+=(-e "TRACE_SIM_PROFILE_INPUTS=${selected_inputs_csv}")
fi
if [ -n "$selected_servers_csv" ]; then
    env_args+=(-e "TRACE_SIM_PROFILE_SERVERS=${selected_servers_csv}")
fi
if [ -n "$container_forced_token_bundle" ]; then
    env_args+=(-e "TRACE_SIM_FORCED_TOKEN_BUNDLE=${container_forced_token_bundle}")
fi
if [ -n "$profile_channels" ]; then
    env_args+=(-e "TRACE_SIM_PROFILE_CHANNELS=${profile_channels}")
fi

docker compose -f "$(compose_file)" run --rm "${env_args[@]}" "$service" \
    bash -lc "$container_cmd"
