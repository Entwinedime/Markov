#!/usr/bin/env bash
# Build the official archive in an image-local copy. The checkout stays untouched.
set -euo pipefail

if (($# != 1)); then
    printf 'usage: %s <trace-sim-root>\n' "$0" >&2
    exit 2
fi

MARKOV_ADAPTER_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
MARKOV_KT_SOURCE=/opt/markov/ktransformers
MARKOV_KT_ARCHIVE="$MARKOV_KT_SOURCE/archive"
mkdir -p "$MARKOV_KT_SOURCE"
cp -a "$1/third_party/ktransformers/." "$MARKOV_KT_SOURCE/"

set +u
source /usr/local/Ascend/ascend-toolkit/set_env.sh
source /usr/local/Ascend/nnal/atb/set_env.sh
set -u
export LD_LIBRARY_PATH="/usr/local/Ascend/driver/lib64/common:/usr/local/Ascend/driver/lib64/driver:${LD_LIBRARY_PATH:-}"
export TASK_QUEUE_ENABLE=0 INF_NAN_MODE_FORCE_DISABLE=1 TORCH_DEVICE_BACKEND_AUTOLOAD=0
export TORCH_NPU_PATH="$(python3 -c 'from importlib.metadata import distribution; print(distribution("torch-npu").locate_file("torch_npu"))')"

# Official gitlinks immediately before upstream moved this runtime into archive.
# Current kt-kernel dependencies belong to a different implementation.
while read -r name repository revision; do
    destination="$MARKOV_KT_ARCHIVE/third_party/$name"
    git init "$destination"
    git -C "$destination" remote add origin "https://github.com/$repository.git"
    git -C "$destination" fetch --depth 1 origin "$revision"
    git -C "$destination" checkout --detach FETCH_HEAD
    git -C "$destination" submodule update --init --recursive --depth 1
done <<'DEPENDENCIES'
PhotonLibOS alibaba/PhotonLibOS 92f56d4527c24aafcee75d87fd72fce25680266f
llama.cpp ggerganov/llama.cpp a94e6ff8774b7c9f950d9545baf0ce35e8d1ed2f
prometheus-cpp jupp0r/prometheus-cpp f13cdd052eeae5e89decc11bf03697d0f78b15bc
pybind11 pybind/pybind11 bb05e0810b87e74709d9f4c4545f1f57a1b386f5
spdlog gabime/spdlog 48bcf39a661a13be22666ac64db8a7f886f2637e
xxHash Cyan4973/xxHash 953a09abc39096da9e216b6eb0002c681cdc1199
DEPENDENCIES

patch --batch -d "$MARKOV_KT_ARCHIVE" -p1 < "$MARKOV_ADAPTER_ROOT/build.patch"
case "$(uname -m)" in
    aarch64|arm64)
        sed -i '/^#define iqk_mul_mat iqk_mul_mat_arm82$/d; /^#define iqk_mul_mat_moe iqk_mul_mat_moe_arm82$/d' \
            "$MARKOV_KT_ARCHIVE/third_party/llamafile/iqk_mul_mat_arm82.cpp"
        ;;
esac
python3 - "$MARKOV_KT_ARCHIVE/ktransformers/configs/config.yaml" <<'PY'
from pathlib import Path
import sys
import yaml

path = Path(sys.argv[1])
configuration = yaml.safe_load(path.read_text(encoding="utf-8"))
configuration["attn"].update(page_size=128, chunk_size=16384)
path.write_text(yaml.safe_dump(configuration, sort_keys=False), encoding="utf-8")
PY

cd "$MARKOV_KT_ARCHIVE"
env -u USE_NUMA USE_BALANCE_SERVE=1 KTRANSFORMERS_FORCE_BUILD=TRUE bash ./install.sh --dev npu
python3 "$MARKOV_ADAPTER_ROOT/runtime.py"
