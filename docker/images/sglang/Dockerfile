ARG BASE_IMAGE=markov-next-sglang-env:ubuntu22.04
FROM ${BASE_IMAGE}

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

COPY src/collection/adapters/frameworks/sglang/ascend /opt/trace-sim/src/collection/adapters/frameworks/sglang/ascend
COPY third_party/sglang /opt/trace-sim/third_party/sglang

RUN bash /opt/trace-sim/src/collection/adapters/frameworks/sglang/ascend/install.sh /opt/trace-sim

COPY src/collection/adapters/runtimes/ascend/torch_npu /opt/trace-sim/src/collection/adapters/runtimes/ascend/torch_npu

ARG MAX_JOBS=16
RUN bash /opt/trace-sim/src/collection/adapters/runtimes/ascend/torch_npu/install.sh

WORKDIR /workspace/trace-sim

CMD ["bash"]
