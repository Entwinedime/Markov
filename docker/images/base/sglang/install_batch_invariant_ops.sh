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

# 加载固定镜像的 CANN 环境，清除可能干扰安装的自定义路径。
set +u
# shellcheck disable=SC1091
source /usr/local/Ascend/ascend-toolkit/set_env.sh
set -u
unset ASCEND_CUSTOM_OPP_PATH

# 从 CANN 环境取得 vendor 路径。
if [ -z "${ASCEND_OPP_PATH:-}" ]; then
    echo "ASCEND_OPP_PATH is not set by CANN set_env.sh" >&2
    exit 1
fi
CUSTOM_OP_ROOT="${ASCEND_OPP_PATH}/vendors/batch_invariant"
CUSTOM_OP_LIB="${CUSTOM_OP_ROOT}/op_api/lib"
CUSTOM_OP_API_SO="${CUSTOM_OP_LIB}/libcust_opapi.so"

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"
cd "$WORK_DIR"

# 安装 AscendC 算子及其 torch extension。
printf '[batch_invariant_ops] Installing AscendC custom ops\n'
curl -fsSL -O "$RUN_URL"
chmod +x "$RUN_PACKAGE"
"./$RUN_PACKAGE"
export ASCEND_CUSTOM_OPP_PATH="$CUSTOM_OP_ROOT"
export LD_LIBRARY_PATH="${CUSTOM_OP_LIB}:${LD_LIBRARY_PATH:-}"

printf '[batch_invariant_ops] Building torch extension\n'
curl -fsSL -O "$EXTENSION_URL"
unzip -q "$EXTENSION_ZIP"
cd torch_ops_extension/batch_invariant_ops
USE_NINJA=1 MAX_JOBS="$(nproc)" python3 setup.py build_ext
USE_NINJA=1 MAX_JOBS="$(nproc)" python3 setup.py bdist_wheel
python3 -m pip install --no-cache-dir --force-reinstall dist/*.whl

# 分别核对 torch 注册与独立加载的 AscendC 符号。
printf '[batch_invariant_ops] Verifying torch.ops symbols\n'
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

printf '[batch_invariant_ops] Verifying AscendC custom op library\n'
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

cd /
rm -rf "$WORK_DIR"
printf '[batch_invariant_ops] Installation complete\n'
