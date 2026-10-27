FROM quay.io/ascend/cann:8.5.0-910b-ubuntu22.04-py3.11 AS dependencies

SHELL ["/bin/bash", "-o", "pipefail", "-c"]
ARG BUILD_JOBS=2
ENV DEBIAN_FRONTEND=noninteractive \
    TORCH_DEVICE_BACKEND_AUTOLOAD=0 \
    MAX_JOBS=${BUILD_JOBS} \
    CMAKE_BUILD_PARALLEL_LEVEL=${BUILD_JOBS} \
    USE_BALANCE_SERVE=1 \
    KTRANSFORMERS_FORCE_BUILD=TRUE \
    TASK_QUEUE_ENABLE=0

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build patch pkg-config \
        libnuma-dev libhwloc-dev libtbb-dev libaio-dev libcurl4-openssl-dev libssl-dev \
    && rm -rf /var/lib/apt/lists/*

# Keep the SDK/PyTorch ABI pair together; the modeling image has neither.
RUN python3 -m pip install --no-cache-dir "setuptools<80" wheel==0.45.1 \
    && python3 -m pip install --no-cache-dir torch==2.6.0 --index-url https://download.pytorch.org/whl/cpu \
    && python3 -m pip install --no-cache-dir \
        torch-npu==2.6.0.post5 numpy==1.26.4 transformers==4.57.1 \
        packaging ninja pybind11 fastapi uvicorn "langchain<0.4" blessed accelerate \
        sentencepiece openai colorlog fire protobuf tiktoken blobfile pyzmq psutil \
        pyyaml scipy decorator attrs sqlalchemy

RUN apt-get update \
    && apt-get install -y --no-install-recommends zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

FROM dependencies AS runtime
COPY third_party/ktransformers/archive /opt/ktransformers
COPY python/markov/adapters/ktransformers/ascend/build.patch /tmp/ktransformers-build.patch
WORKDIR /opt/ktransformers
RUN patch -p1 < /tmp/ktransformers-build.patch \
    && export TORCH_NPU_PATH="$(python3 -c 'import importlib.util; from pathlib import Path; print(Path(importlib.util.find_spec("torch_npu").origin).parent)')" \
    && python3 -m pip install --no-deps --no-build-isolation . \
    && rm -f /tmp/ktransformers-build.patch

ENV OMP_NUM_THREADS=2 OPENBLAS_NUM_THREADS=2 MKL_NUM_THREADS=2
WORKDIR /workspace/markov
CMD ["bash"]
