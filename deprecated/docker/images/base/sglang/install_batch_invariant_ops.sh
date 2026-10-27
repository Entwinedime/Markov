#!/usr/bin/env bash
# 安装 SGLang #21197 所需的 Ascend NPU batch_invariant_ops。
#
# 该脚本只服务于固定的 910b/aarch64 Docker base 镜像，因此不保留 vLLM-Ascend
# 安装脚本中的平台分支和自动识别逻辑。
set -euo pipefail

readonly WORK_DIR="/tmp/batch-invariant-ops"
readonly RUN_PACKAGE="cann-ops-batch_invariant-910b-1.0.0-linux.aarch64.run"
readonly RUN_URL="https://vllm-ascend.obs.cn-north-4.myhuaweicloud.com/vllm-ascend/${RUN_PACKAGE}"
readonly EXTENSION_ZIP="batch_invariant-torch_ops_extension-1.0.0.zip"
readonly EXTENSION_URL="https://vllm-ascend.obs.cn-north-4.myhuaweicloud.com/vllm-ascend/${EXTENSION_ZIP}"

CUSTOM_OP_ROOT=""
CUSTOM_OP_LIB=""
CUSTOM_OP_API_SO=""

log() {
    echo "[batch_invariant_ops] $*"
}

# 加载 CANN 编译环境，并避免已有自定义 OPP 路径影响 run 包安装。
prepare_environment() {
    set +u
    # shellcheck disable=SC1091
    source /usr/local/Ascend/ascend-toolkit/set_env.sh
    set -u
    unset ASCEND_CUSTOM_OPP_PATH
}

# 从 CANN 环境推导 vendor 路径，避免把具体 CANN 安装目录写死在脚本中。
configure_custom_op_paths() {
    if [ -z "${ASCEND_OPP_PATH:-}" ]; then
        echo "ASCEND_OPP_PATH is not set by CANN set_env.sh" >&2
        exit 1
    fi
    CUSTOM_OP_ROOT="${ASCEND_OPP_PATH}/vendors/batch_invariant"
    CUSTOM_OP_LIB="${CUSTOM_OP_ROOT}/op_api/lib"
    CUSTOM_OP_API_SO="${CUSTOM_OP_LIB}/libcust_opapi.so"
}

# 安装 AscendC 自定义算子 run 包。
install_custom_ops() {
    log "Installing AscendC custom ops"
    curl -fsSL -k -O "$RUN_URL"
    chmod +x "$RUN_PACKAGE"
    "./$RUN_PACKAGE"
    export ASCEND_CUSTOM_OPP_PATH="$CUSTOM_OP_ROOT"
    export LD_LIBRARY_PATH="${CUSTOM_OP_LIB}:${LD_LIBRARY_PATH:-}"
}

# 构建并安装 torch extension。该 wheel 负责注册 torch.ops.batch_invariant_ops。
install_torch_extension() {
    log "Building torch extension"
    curl -fsSL -k -O "$EXTENSION_URL"
    unzip -q "$EXTENSION_ZIP"
    cd torch_ops_extension/batch_invariant_ops
    USE_NINJA=1 MAX_JOBS="$(nproc)" python3 setup.py build_ext
    USE_NINJA=1 MAX_JOBS="$(nproc)" python3 setup.py bdist_wheel
    python3 -m pip install --no-cache-dir --force-reinstall dist/*.whl
}

# 在构建时确认 SGLang deterministic NPU wrapper 需要的符号已经注册。
verify_installation() {
    log "Verifying torch.ops symbols"
    python3 - <<'PY'
import batch_invariant_ops  # noqa: F401
import torch

for name in (
    "npu_mm_batch_invariant",
    "npu_matmul_batch_invariant",
    "npu_reduce_mean_batch_invariant",
    "npu_log_softmax_batch_invariant",
    "npu_fused_infer_attention_score_batch_invariant",
):
    getattr(torch.ops.batch_invariant_ops, name)
PY

    log "Verifying AscendC custom op library"
    CUSTOM_OP_API_SO="$CUSTOM_OP_API_SO" python3 - <<'PY'
import ctypes
import os

lib = ctypes.CDLL(os.environ["CUSTOM_OP_API_SO"], mode=os.RTLD_NOW)
for name in (
    "aclnnMmBatchInvariant",
    "aclnnMmBatchInvariantGetWorkspaceSize",
    "aclnnMatmulBatchInvariant",
    "aclnnMatmulBatchInvariantGetWorkspaceSize",
    "aclnnReduceMeanBatchInvariant",
    "aclnnReduceMeanBatchInvariantGetWorkspaceSize",
    "aclnnLogSoftmaxBatchInvariant",
    "aclnnLogSoftmaxBatchInvariantGetWorkspaceSize",
):
    getattr(lib, name)
PY
}

main() {
    prepare_environment
    configure_custom_op_paths
    rm -rf "$WORK_DIR"
    mkdir -p "$WORK_DIR"
    cd "$WORK_DIR"

    install_custom_ops
    install_torch_extension
    verify_installation

    cd /
    rm -rf "$WORK_DIR"
    log "Installation complete"
}

main "$@"
