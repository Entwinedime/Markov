#!/usr/bin/env bash
# Install the Ascend NPU batch_invariant_ops required by SGLang #21197.
#
# The pinned 910B AArch64 base image determines the platform.
set -euo pipefail

readonly WORK_DIR="/tmp/batch-invariant-ops"
readonly RUN_PACKAGE="cann-ops-batch_invariant-910b-1.0.0-linux.aarch64.run"
readonly RUN_URL="https://vllm-ascend.obs.cn-north-4.myhuaweicloud.com/vllm-ascend/${RUN_PACKAGE}"
readonly EXTENSION_ZIP="batch_invariant-torch_ops_extension-1.0.0.zip"
readonly EXTENSION_URL="https://vllm-ascend.obs.cn-north-4.myhuaweicloud.com/vllm-ascend/${EXTENSION_ZIP}"

# Use the image's CANN environment and clear any custom operator path.
set +u
# shellcheck disable=SC1091
source /usr/local/Ascend/ascend-toolkit/set_env.sh
set -u
unset ASCEND_CUSTOM_OPP_PATH

# Resolve the operator's vendor directory from the CANN environment.
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

# Install the AscendC operators and their PyTorch extension.
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

# Verify the PyTorch registrations and the AscendC library exports separately.
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
