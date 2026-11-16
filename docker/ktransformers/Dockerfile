FROM quay.io/ascend/sglang@sha256:5ed0acf504181fe092f9badbd8f77611b985ec65df0f93b66ab4b9616231f70b

SHELL ["/bin/bash", "-o", "pipefail", "-c"]
ENV DEBIAN_FRONTEND=noninteractive \
    DSV4_WORKSPACE=/opt/dsv4 \
    KTRANSFORMERS_REPO=/opt/dsv4/ktransformers \
    SGLANG_REPO=/opt/dsv4/sglang \
    DSV4_MODEL_ROOT=/models \
    DSV4_SOC=ascend910b \
    DSV4_JOBS=8

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       build-essential cmake git libhwloc-dev libhwloc15 libnuma-dev patchelf pkg-config \
    && rm -rf /var/lib/apt/lists/*

RUN git init "$KTRANSFORMERS_REPO" \
    && git -C "$KTRANSFORMERS_REPO" remote add origin https://github.com/kvcache-ai/ktransformers.git \
    && git -C "$KTRANSFORMERS_REPO" fetch --depth 1 origin ca198c50de70abb6827ba6f573bb6671e54436e2 \
    && git -C "$KTRANSFORMERS_REPO" checkout --detach FETCH_HEAD \
    && git -C "$KTRANSFORMERS_REPO" submodule update --init --depth 1 third_party/llama.cpp third_party/pybind11 \
    && git init "$SGLANG_REPO" \
    && git -C "$SGLANG_REPO" remote add origin https://github.com/Pan-Boyi/sglang.git \
    && git -C "$SGLANG_REPO" fetch --depth 1 origin ff46d660d16cbf11ef393c80cc6b3ab7f6259780 \
    && git -C "$SGLANG_REPO" checkout --detach FETCH_HEAD

RUN bash "$KTRANSFORMERS_REPO/kt-kernel/tools/ascend_dsv4/setup.sh" deps \
    && bash "$KTRANSFORMERS_REPO/kt-kernel/tools/ascend_dsv4/setup.sh" kt-kernel

COPY pyproject.toml /opt/markov/pyproject.toml
COPY python /opt/markov/python
RUN python3 -m pip install --no-index --no-deps --no-build-isolation /opt/markov
COPY docker/ktransformers/entrypoint.sh /opt/markov/entrypoint.sh
ENV SGLANG_PLUGINS=markov_kt_facts

WORKDIR /opt/dsv4
ENTRYPOINT ["bash", "/opt/markov/entrypoint.sh"]
CMD ["bash"]
