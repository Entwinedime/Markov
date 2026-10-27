FROM quay.io/ascend/cann:8.3.rc1-910b-ubuntu22.04-py3.11

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

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
        libaio-dev \
        libcurl4-openssl-dev \
        libffi-dev \
        libglib2.0-0 \
        libhwloc-dev \
        libnuma-dev \
        libssl-dev \
        libtbb-dev \
        locales \
        lld \
        make \
        ninja-build \
        patch \
        pkg-config \
        python3 \
        python3-dev \
        python3-pip \
        python3-venv \
        rsync \
        unzip \
        wget \
        zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

RUN locale-gen en_US.UTF-8

ENV LANG=en_US.UTF-8
ENV LANGUAGE=en_US:en
ENV LC_ALL=en_US.UTF-8
ENV ASCEND_HOME_PATH=/usr/local/Ascend/ascend-toolkit/latest
ENV ASCEND_TOOLKIT_HOME=/usr/local/Ascend/ascend-toolkit/latest
ENV LD_LIBRARY_PATH=/usr/local/Ascend/ascend-toolkit/latest/lib64:/usr/local/Ascend/driver/lib64/common:/usr/local/Ascend/driver/lib64/driver:${LD_LIBRARY_PATH}

RUN python3 -m pip config set global.index-url https://pypi.org/simple/ \
    && python3 -m pip install --no-cache-dir --upgrade pip \
    && python3 -m pip install --no-cache-dir "setuptools<80" wheel==0.45.1 \
    && python3 -m pip install --no-cache-dir \
        torch==2.5.1 \
        torchvision==0.20.1 \
        torchaudio==2.5.1 \
        --index-url https://download.pytorch.org/whl/cpu \
    && python3 -m pip install --no-cache-dir \
        numpy==1.26.4 \
        packaging \
        ninja \
        fire \
        protobuf \
        attrs \
        decorator \
        cloudpickle \
        ml-dtypes \
        scipy \
        tornado \
        absl-py \
        psutil \
        sqlalchemy \
        transformers==4.57.1 \
        pyyaml \
        typing-extensions \
        pybind11

RUN set -euo pipefail; \
    set +u; \
    source /usr/local/Ascend/ascend-toolkit/set_env.sh; \
    source /usr/local/Ascend/nnal/atb/set_env.sh; \
    set -u; \
    rm -rf /opt/torch_npu; \
    git clone --recursive --depth 1 --branch v2.5.1 https://gitcode.com/Ascend/pytorch.git /opt/torch_npu; \
    cd /opt/torch_npu; \
    bash ci/build.sh --python=3.11; \
    wheel_path="$(find /opt/torch_npu/dist -maxdepth 1 -type f -name 'torch_npu*.whl' | sort | tail -n 1)"; \
    test -n "$wheel_path"; \
    python3 -m pip install --no-cache-dir --no-deps --force-reinstall "$wheel_path"; \
    version_file="$(python3 -c 'import site; from pathlib import Path; matches=[str(Path(site_dir) / "torch_npu" / "version.py") for site_dir in site.getsitepackages() if (Path(site_dir) / "torch_npu" / "version.py").is_file()]; print(matches[0] if matches else "")')"; \
    test -n "$version_file"; \
    VERSION_FILE="$version_file" python3 -c "import os, re; from pathlib import Path; path=Path(os.environ['VERSION_FILE']); text=path.read_text(encoding='utf-8'); new=re.sub(r\"(__version__\\s*=\\s*['\\\"])([^'\\\"]+)\\+[^'\\\"]+(['\\\"])\", r\"\\1\\2\\3\", text); path.write_text(new, encoding='utf-8') if new != text else None"; \
    cd /tmp; \
    TORCH_DEVICE_BACKEND_AUTOLOAD=0 python3 -c "import torch; print(f'torch={torch.__version__}')"; \
    VERSION_FILE="$version_file" python3 -c "import os; from pathlib import Path; namespace={}; exec(Path(os.environ['VERSION_FILE']).read_text(encoding='utf-8'), namespace); print(f'torch_npu={namespace.get(\"__version__\", \"unknown\")}')"; \
    rm -rf /opt/torch_npu

WORKDIR /workspace/trace-sim

CMD ["bash"]
