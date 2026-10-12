ARG BASE_IMAGE=markov-next-sglang-env:ubuntu22.04
FROM ${BASE_IMAGE}

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

COPY docker/install/sglang.sh /opt/trace-sim/install/sglang.sh
COPY src/collection/adapters/frameworks/sglang/ascend /opt/trace-sim/src/collection/adapters/frameworks/sglang/ascend
COPY third_party/sglang /opt/trace-sim/third_party/sglang

RUN chmod +x /opt/trace-sim/install/sglang.sh \
    && /opt/trace-sim/install/sglang.sh /opt/trace-sim

COPY docker/install/torch_npu_workspace.sh /opt/trace-sim/docker/install/
COPY src/collection/adapters/runtimes/ascend/torch_npu /opt/trace-sim/src/collection/adapters/runtimes/ascend/torch_npu

ARG MAX_JOBS=16
RUN bash /opt/trace-sim/docker/install/torch_npu_workspace.sh /opt/trace-sim

WORKDIR /workspace/trace-sim

CMD ["bash"]
