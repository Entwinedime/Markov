ARG BASE_IMAGE=markov-next-sglang-env:ubuntu22.04
FROM ${BASE_IMAGE}

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

COPY src/collection/adapters/frameworks/sglang/ascend /opt/trace-sim/src/collection/adapters/frameworks/sglang/ascend
COPY third_party/sglang /opt/trace-sim/third_party/sglang

RUN bash /opt/trace-sim/src/collection/adapters/frameworks/sglang/ascend/install.sh /opt/trace-sim

COPY pyproject.toml /opt/markov/collector/pyproject.toml
COPY src/collection/markov_collection /opt/markov/collector/src/collection/markov_collection
RUN python3 -m pip install --no-deps --no-build-isolation /opt/markov/collector

WORKDIR /workspace/trace-sim

CMD ["bash"]
