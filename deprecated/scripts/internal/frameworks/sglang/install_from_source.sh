#!/usr/bin/env bash
# Install the SGLang NPU package from submodule source in the runtime image.
#
# The Dockerfile invokes this script after copying or mounting the repository;
# submodule initialization is intentionally outside this boundary.
set -euo pipefail

if [ $# -ne 1 ]; then
    echo "usage: $0 <trace-sim-root>" >&2
    exit 2
fi

TRACE_SIM_ROOT="$1"
SGLANG_SRC="${TRACE_SIM_ROOT}/third_party/sglang"

# Emit timestamped installation messages.
log() {
    echo "[$(date +"%Y-%m-%d %H:%M:%S")] $*"
}

# Load the Ascend toolkit and complete the driver library path.
source_ascend_env() {
    set +u
    # shellcheck disable=SC1091
    source /usr/local/Ascend/ascend-toolkit/set_env.sh
    set -u

    export LD_LIBRARY_PATH="/usr/local/Ascend/driver/lib64/common:/usr/local/Ascend/driver/lib64/driver:${LD_LIBRARY_PATH:-}"
}

# Install the NPU project metadata at the standard pyproject.toml path.
prepare_sglang_pyproject() {
    local pyproject_dir="${SGLANG_SRC}/python"
    local npu_pyproject="${pyproject_dir}/pyproject_npu.toml"

    if [ ! -f "$npu_pyproject" ]; then
        log "Missing SGLang NPU pyproject: ${npu_pyproject}"
        exit 1
    fi

    rm -f "${pyproject_dir}/pyproject.toml"
    cp "$npu_pyproject" "${pyproject_dir}/pyproject.toml"
}

# Install the SGLang Python package and NPU dependencies in editable mode.
install_sglang() {
    log "Installing SGLang: ${SGLANG_SRC}"
    prepare_sglang_pyproject
    (
        cd "${SGLANG_SRC}/python"
        python3 -m pip install --no-cache-dir -v -e ".[all_npu]"
    )
}

# Validate source layout, initialize the environment, and install.
main() {
    source_ascend_env

    if [ ! -d "$SGLANG_SRC" ]; then
        log "Missing SGLang source tree: ${SGLANG_SRC}"
        exit 1
    fi

    install_sglang
}

main "$@"
