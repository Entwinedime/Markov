#!/usr/bin/env bash
# 在 runtime 镜像中从子模块源码安装 KTransformers archive NPU runtime。
#
# KTransformers 当前依赖 archive 子树和若干源码补丁；这些补丁只修正构建
# 入口和 NPU 选择逻辑，不生成 profiling fixture。
set -euo pipefail

if [ $# -ne 1 ]; then
    echo "usage: $0 <trace-sim-root>" >&2
    exit 2
fi

TRACE_SIM_ROOT="$1"
KTRANSFORMERS_SRC="${TRACE_SIM_ROOT}/third_party/ktransformers"
KTRANSFORMERS_RUNTIME_SRC="${KTRANSFORMERS_SRC}/archive"

# 输出带时间戳的安装日志。
log() {
    echo "[$(date +"%Y-%m-%d %H:%M:%S")] $*"
}

# 加载 Ascend toolkit/ATB 环境并设置 runtime 必需变量。
source_ascend_env() {
    set +u
    # shellcheck disable=SC1091
    source /usr/local/Ascend/ascend-toolkit/set_env.sh
    # shellcheck disable=SC1091
    source /usr/local/Ascend/nnal/atb/set_env.sh
    set -u

    export LD_LIBRARY_PATH="/usr/local/Ascend/driver/lib64/common:/usr/local/Ascend/driver/lib64/driver:${LD_LIBRARY_PATH:-}"
    export TASK_QUEUE_ENABLE=0
    export INF_NAN_MODE_FORCE_DISABLE=1
}

# 验证 torch 与 torch_npu 可导入，并输出版本信息。
check_torch_npu() {
    TORCH_DEVICE_BACKEND_AUTOLOAD=0 python3 - <<'PY'
from pathlib import Path
import site
import torch

print(f"torch={torch.__version__}")

version_files = [
    Path(site_dir) / "torch_npu" / "version.py"
    for site_dir in site.getsitepackages()
]
version_file = next((path for path in version_files if path.is_file()), None)
if version_file is None:
    raise SystemExit("torch_npu version.py is missing")

namespace = {}
exec(version_file.read_text(encoding="utf-8"), namespace)
print(f"torch_npu={namespace.get('__version__', 'unknown')}")
PY
}

# 在 ARM 平台禁用会冲突的 llamafile arm82 宏别名。
patch_arm82_llamafile() {
    case "$(uname -m)" in
        aarch64|arm64) ;;
        *) return 0 ;;
    esac

    local file
    for file in \
        "${KTRANSFORMERS_RUNTIME_SRC}/third_party/llamafile/iqk_mul_mat_arm82.cpp" \
        "${KTRANSFORMERS_SRC}/third_party/llamafile/iqk_mul_mat_arm82.cpp"; do
        if [ ! -f "$file" ]; then
            continue
        fi

        log "Patching arm82 llamafile aliases: ${file}"
        python3 - "$file" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8")

lines = []
for line in text.splitlines(keepends=True):
    stripped = line.strip()
    if stripped in {
        "#define iqk_mul_mat iqk_mul_mat_arm82",
        "#define iqk_mul_mat_moe iqk_mul_mat_moe_arm82",
    } and not line.lstrip().startswith("//"):
        indent = line[: len(line) - len(line.lstrip())]
        line = f"{indent}// TRACE_SIM_ASCEND_DISABLED {line.lstrip()}"
    lines.append(line)

path.write_text("".join(lines), encoding="utf-8")
PY
    done
}

# 固定 archive 默认 attention page/chunk 配置，匹配当前运行环境。
patch_archive_config() {
    local config="${KTRANSFORMERS_RUNTIME_SRC}/ktransformers/configs/config.yaml"
    if [ ! -f "$config" ]; then
        log "Missing ktransformers archive config: ${config}"
        exit 1
    fi

    log "Patching archive config attn.page_size=128, attn.chunk_size=16384"
    python3 - "$config" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8")

lines = text.splitlines()
out = []
in_attn = False
attn_indent = None
seen_page = False
seen_chunk = False

def emit_missing():
    emitted = []
    if not seen_page:
        emitted.append("  page_size: 128")
    if not seen_chunk:
        emitted.append("  chunk_size: 16384")
    return emitted

for line in lines:
    stripped = line.strip()
    indent = len(line) - len(line.lstrip(" "))

    if in_attn and stripped and indent <= attn_indent and not line.startswith(" " * (attn_indent + 1)):
        out.extend(emit_missing())
        in_attn = False

    if stripped == "attn:":
        in_attn = True
        attn_indent = indent
        seen_page = False
        seen_chunk = False
        out.append(line)
        continue

    if in_attn and stripped.startswith("page_size:"):
        out.append(" " * (attn_indent + 2) + "page_size: 128")
        seen_page = True
        continue

    if in_attn and stripped.startswith("chunk_size:"):
        out.append(" " * (attn_indent + 2) + "chunk_size: 16384")
        seen_chunk = True
        continue

    out.append(line)

if in_attn:
    out.extend(emit_missing())

path.write_text("\n".join(out) + "\n", encoding="utf-8")
PY
}

# 允许通过环境变量显式选择 NPU build，避免安装期误判。
patch_archive_setup_npu_backend() {
    local setup_py="${KTRANSFORMERS_RUNTIME_SRC}/setup.py"
    if [ ! -f "$setup_py" ]; then
        log "Missing ktransformers archive setup.py: ${setup_py}"
        exit 1
    fi

    log "Patching archive setup.py to allow explicit NPU build selection"
    python3 - "$setup_py" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8")

old = '''try:
    import torch_npu
    KTRANSFORMERS_BUILD_NPU = torch_npu.npu.is_available()
except:
    KTRANSFORMERS_BUILD_NPU = False
'''

new = '''def _trace_sim_torch_npu_version():
    import site
    from pathlib import Path

    for site_dir in site.getsitepackages():
        version_file = Path(site_dir) / "torch_npu" / "version.py"
        if not version_file.is_file():
            continue
        namespace = {}
        exec(version_file.read_text(encoding="utf-8"), namespace)
        return namespace.get("__version__", "unknown")
    return "unknown"


if os.environ.get("KTRANSFORMERS_BUILD_NPU", "0") == "1":
    class _TraceSimTorchNpu:
        __version__ = _trace_sim_torch_npu_version()

    torch_npu = _TraceSimTorchNpu()
    KTRANSFORMERS_BUILD_NPU = True
else:
    try:
        import torch_npu
        KTRANSFORMERS_BUILD_NPU = torch_npu.npu.is_available()
    except:
        KTRANSFORMERS_BUILD_NPU = False
'''

if old not in text:
    if "_trace_sim_torch_npu_version" in text:
        raise SystemExit(0)
    raise SystemExit("target torch_npu detection block not found")

path.write_text(text.replace(old, new), encoding="utf-8")
PY
}

# 让 balance_serve 优先使用外部传入的 TORCH_NPU_PATH。
patch_balance_serve_torch_npu_lookup() {
    local cmake_file="${KTRANSFORMERS_RUNTIME_SRC}/csrc/balance_serve/CMakeLists.txt"
    if [ ! -f "$cmake_file" ]; then
        log "Missing balance_serve CMakeLists.txt: ${cmake_file}"
        exit 1
    fi

    log "Patching balance_serve torch_npu lookup to use TORCH_NPU_PATH"
    python3 - "$cmake_file" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8")

old = '''    # find torch_npu
    execute_process(
            COMMAND python -c "import torch; import torch_npu; print(torch_npu.__path__[0])"
            OUTPUT_VARIABLE TORCH_NPU_PATH
            OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    message(STATUS "Found PTA at: ${TORCH_NPU_PATH}")
    find_library(PTA_LIBRARY torch_npu PATH "${TORCH_NPU_PATH}/lib")
'''

new = '''    # find torch_npu
    if(DEFINED ENV{TORCH_NPU_PATH})
        set(TORCH_NPU_PATH "$ENV{TORCH_NPU_PATH}")
    else()
        execute_process(
                COMMAND python -c "import torch; import torch_npu; print(torch_npu.__path__[0])"
                OUTPUT_VARIABLE TORCH_NPU_PATH
                OUTPUT_STRIP_TRAILING_WHITESPACE
        )
    endif()
    message(STATUS "Found PTA at: ${TORCH_NPU_PATH}")
    find_library(PTA_LIBRARY torch_npu PATH "${TORCH_NPU_PATH}/lib")
'''

if old not in text:
    if "DEFINED ENV{TORCH_NPU_PATH}" in text:
        raise SystemExit(0)
    raise SystemExit("target torch_npu lookup block not found")

path.write_text(text.replace(old, new), encoding="utf-8")
PY
}

# 执行 KTransformers archive runtime 安装。
install_archive_runtime() {
    if [ ! -f "${KTRANSFORMERS_RUNTIME_SRC}/install.sh" ]; then
        log "Missing ktransformers archive install script: ${KTRANSFORMERS_RUNTIME_SRC}/install.sh"
        exit 1
    fi

    local torch_npu_path
    torch_npu_path="$(python3 - <<'PY'
from pathlib import Path
import site

for site_dir in site.getsitepackages():
    path = Path(site_dir) / "torch_npu"
    if (path / "lib" / "libtorch_npu.so").is_file():
        print(path)
        break
PY
)"
    if [ -z "$torch_npu_path" ]; then
        log "Missing torch_npu library path"
        exit 1
    fi

    rm -rf "${KTRANSFORMERS_RUNTIME_SRC}/csrc/balance_serve/build"

    log "Installing ktransformers archive runtime: ${KTRANSFORMERS_RUNTIME_SRC}"
    (
        cd "$KTRANSFORMERS_RUNTIME_SRC"
        env -u USE_NUMA \
            TORCH_DEVICE_BACKEND_AUTOLOAD=0 \
            TORCH_NPU_PATH="$torch_npu_path" \
            USE_BALANCE_SERVE=1 \
            KTRANSFORMERS_BUILD_NPU=1 \
            KTRANSFORMERS_FORCE_BUILD=TRUE \
            bash ./install.sh --dev cuda
    )
}

# 安装入口：校验源码树、应用构建补丁并执行安装。
main() {
    if [ ! -d "$KTRANSFORMERS_SRC" ]; then
        log "Missing ktransformers source tree: ${KTRANSFORMERS_SRC}"
        exit 1
    fi
    if [ ! -d "$KTRANSFORMERS_RUNTIME_SRC" ]; then
        log "Missing ktransformers archive runtime: ${KTRANSFORMERS_RUNTIME_SRC}"
        exit 1
    fi

    source_ascend_env
    check_torch_npu
    patch_arm82_llamafile
    patch_archive_config
    patch_archive_setup_npu_backend
    patch_balance_serve_torch_npu_lookup
    install_archive_runtime
}

main "$@"
