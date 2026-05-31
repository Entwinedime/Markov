#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/lib/common.sh"

ROOT_DIR="$(repo_root)"
cd "$ROOT_DIR"

usage() {
    cat >&2 <<'EOF'
usage:
  scripts/profile.sh <config.json> [--dry-run]

Runs a JSON-configured profiling experiment inside the framework container.
The config chooses the framework, server command, workload, hook settings, and
SGLang /start_profile body.
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
while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run)
            dry_run=1
            shift
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
python3 scripts/internal/profile_runner.py --config "$TRACE_SIM_PROFILE_CONFIG" ${TRACE_SIM_PROFILE_DRY_RUN:+--dry-run}
'

env_args=(
    -e "TRACE_SIM_PROFILE_CONFIG=${container_config}"
    -e "TRACE_SIM_HOOK_ASCENDCL_SO_PATH=${ascendcl_so_path}"
)
if [ "$dry_run" = "1" ]; then
    env_args+=(-e "TRACE_SIM_PROFILE_DRY_RUN=1")
fi

docker compose -f "$(compose_file)" run --rm "${env_args[@]}" "$service" \
    bash -lc "$container_cmd"
