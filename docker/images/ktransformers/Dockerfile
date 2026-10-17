ARG BASE_IMAGE=markov-next-ktransformers-env:ubuntu22.04
FROM ${BASE_IMAGE}

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

COPY src/collection/adapters/frameworks/ktransformers/ascend /opt/trace-sim/src/collection/adapters/frameworks/ktransformers/ascend
COPY third_party/ktransformers /opt/trace-sim/third_party/ktransformers

RUN bash /opt/trace-sim/src/collection/adapters/frameworks/ktransformers/ascend/install.sh /opt/trace-sim

WORKDIR /workspace/trace-sim

CMD ["bash"]
