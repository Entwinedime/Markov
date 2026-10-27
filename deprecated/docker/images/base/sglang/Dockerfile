ARG CANN_VERSION=9.0.0
ARG DEVICE_TYPE=910b
ARG OS=ubuntu22.04
ARG PYTHON_VERSION=py3.11

FROM quay.io/ascend/cann:${CANN_VERSION}-${DEVICE_TYPE}-${OS}-${PYTHON_VERSION}

ARG CANN_VERSION
ARG DEVICE_TYPE
ARG PYTORCH_VERSION=2.10.0
ARG TORCHVISION_VERSION=0.25.0
ARG TORCHAUDIO_VERSION=2.10.0
ARG SGLANG_KERNEL_NPU_TAG=2026.05.01.post3
ARG PIP_INSTALL="python3 -m pip install --no-cache-dir"

RUN DEBIAN_FRONTEND=noninteractive apt-get update \
    && apt-get install -y --no-install-recommends \
        bash \
        build-essential \
        ca-certificates \
        ccache \
        clang \
        cmake \
        curl \
        g++ \
        gcc \
        git \
        libgl1 \
        libglib2.0-0 \
        libssl-dev \
        locales \
        lld \
        make \
        ninja-build \
        pkg-config \
        python3 \
        python3-dev \
        python3-pip \
        python3-venv \
        unzip \
        wget \
    && rm -rf /var/lib/apt/lists/*

RUN locale-gen en_US.UTF-8

ENV LANG=en_US.UTF-8
ENV LANGUAGE=en_US:en
ENV LC_ALL=en_US.UTF-8

ENV PIP_INDEX_URL=https://pypi.tuna.tsinghua.edu.cn/simple

RUN python3 -m pip config set global.index-url https://pypi.org/simple/ \
    && ${PIP_INSTALL} --upgrade pip \
    && ${PIP_INSTALL} "setuptools<80" wheel==0.45.1 \
    && ${PIP_INSTALL} \
        torch==${PYTORCH_VERSION} \
        torchvision==${TORCHVISION_VERSION} \
        torchaudio==${TORCHAUDIO_VERSION} \
        --index-url https://download.pytorch.org/whl/cpu \
    && ${PIP_INSTALL} \
        memfabric-hybrid==1.0.8 \
        sglang-router \
        pybind11 \
        pyyaml \
        decorator \
        scipy \
        attrs \
        psutil \
    && ${PIP_INSTALL} \
        https://gitcode.com/Ascend/pytorch/releases/download/v26.0.0-pytorch2.10.0/torch_npu-2.10.0-cp311-cp311-manylinux_2_28_aarch64.whl \
        memfabric-zbal==1.1.1 \
    && ${PIP_INSTALL} \
        triton-ascend==3.2.1.dev20260530 \
        --extra-index-url=https://mirrors.huaweicloud.com/ascend/repos/pypi/nightly \
        --trusted-host triton-ascend.osinfra.cn

RUN mkdir /tmp/sgl-kernel-npu \
    && cd /tmp/sgl-kernel-npu \
    && wget https://github.com/sgl-project/sgl-kernel-npu/releases/download/${SGLANG_KERNEL_NPU_TAG}/sgl-kernel-npu-${SGLANG_KERNEL_NPU_TAG}-torch${PYTORCH_VERSION}-py311-cann${CANN_VERSION}-${DEVICE_TYPE}-$(arch).zip \
    && unzip sgl-kernel-npu-${SGLANG_KERNEL_NPU_TAG}-torch${PYTORCH_VERSION}-py311-cann${CANN_VERSION}-${DEVICE_TYPE}-$(arch).zip \
    && ${PIP_INSTALL} deep_ep*.whl sgl_kernel_npu*.whl \
    && cd "$(python3 -m pip show deep-ep | awk '/^Location:/ {print $2}')" \
    && ln -sf deep_ep/deep_ep_cpp*.so \
    && rm -rf /tmp/sgl-kernel-npu

COPY install_batch_invariant_ops.sh /tmp/install_batch_invariant_ops.sh
RUN chmod +x /tmp/install_batch_invariant_ops.sh \
    && /tmp/install_batch_invariant_ops.sh \
    && rm -f /tmp/install_batch_invariant_ops.sh

WORKDIR /workspace/trace-sim

CMD ["bash"]
