FROM quay.io/ascend/cann:8.5.0-910b-ubuntu22.04-py3.11

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

RUN python3 -m pip config set global.index-url https://pypi.org/simple/ \
    && python3 -m pip install --no-cache-dir --upgrade pip \
    && python3 -m pip install --no-cache-dir "setuptools<80" wheel==0.45.1 \
    && python3 -m pip install --no-cache-dir \
        torch==2.10.0 \
        torchvision==0.25.0 \
        torchaudio==2.10.0 \
        --index-url https://download.pytorch.org/whl/cpu \
    && python3 -m pip install --no-cache-dir \
        memfabric-hybrid==1.0.8 \
        sglang-router \
        pybind11 \
        pyyaml \
        decorator \
        scipy \
        attrs \
        psutil \
    && python3 -m pip install --no-cache-dir \
        https://gitcode.com/Ascend/pytorch/releases/download/v26.0.0-pytorch2.10.0/torch_npu-2.10.0-cp311-cp311-manylinux_2_28_aarch64.whl \
        https://gitcode.com/Ascend/triton-ascend/releases/download/v3.2.1/triton_ascend-3.2.1-cp311-cp311-manylinux_2_27_aarch64.manylinux_2_28_aarch64.whl \
        memfabric-zbal==1.1.1

RUN mkdir /tmp/sgl-kernel-npu \
    && cd /tmp/sgl-kernel-npu \
    && wget https://github.com/sgl-project/sgl-kernel-npu/releases/download/2026.05.01.post3/sgl-kernel-npu-2026.05.01.post3-torch2.10.0-py311-cann8.5.0-910b-$(arch).zip \
    && unzip sgl-kernel-npu-2026.05.01.post3-torch2.10.0-py311-cann8.5.0-910b-$(arch).zip \
    && python3 -m pip install --no-cache-dir deep_ep*.whl sgl_kernel_npu*.whl \
    && cd "$(python3 -m pip show deep-ep | awk '/^Location:/ {print $2}')" \
    && ln -sf deep_ep/deep_ep_cpp*.so \
    && rm -rf /tmp/sgl-kernel-npu

WORKDIR /workspace/trace-sim

CMD ["bash"]
