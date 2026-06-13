#!/usr/bin/env bash
# 在 runtime 镜像中从子模块源码安装 SGLang NPU 版本。
#
# 该脚本由 Dockerfile 调用，假设仓库已经挂载/复制到镜像内，并且不负责
# 初始化子模块。
set -euo pipefail

if [ $# -ne 1 ]; then
    echo "usage: $0 <trace-sim-root>" >&2
    exit 2
fi

TRACE_SIM_ROOT="$1"
SGLANG_SRC="${TRACE_SIM_ROOT}/third_party/sglang"

# 输出带时间戳的安装日志。
log() {
    echo "[$(date +"%Y-%m-%d %H:%M:%S")] $*"
}

# 加载 Ascend toolkit 环境并补齐 driver library path。
source_ascend_env() {
    set +u
    # shellcheck disable=SC1091
    source /usr/local/Ascend/ascend-toolkit/set_env.sh
    set -u

    export LD_LIBRARY_PATH="/usr/local/Ascend/driver/lib64/common:/usr/local/Ascend/driver/lib64/driver:${LD_LIBRARY_PATH:-}"
}

# 把 SGLang NPU pyproject 切换为标准 pyproject.toml。
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

# 以 editable 方式安装 SGLang Python 包和 NPU 依赖。
install_sglang() {
    log "Installing SGLang: ${SGLANG_SRC}"
    prepare_sglang_pyproject
    (
        cd "${SGLANG_SRC}/python"
        python3 -m pip install --no-cache-dir -v -e ".[all_npu]"
    )
}

# 安装入口：检查源码树、加载环境并执行安装。
main() {
    source_ascend_env

    if [ ! -d "$SGLANG_SRC" ]; then
        log "Missing SGLang source tree: ${SGLANG_SRC}"
        exit 1
    fi

    install_sglang
}

main "$@"
