ARG BASE_IMAGE=markov-next-sglang-env:ubuntu22.04
FROM ${BASE_IMAGE}

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

COPY src/collection/native/frameworks/sglang/ascend/install.sh /opt/markov/install-sglang.sh
COPY third_party/sglang /opt/markov/sglang

RUN bash /opt/markov/install-sglang.sh /opt/markov/sglang && rm /opt/markov/install-sglang.sh

COPY pyproject.toml /opt/markov/collector/pyproject.toml
COPY src/collection/markov_collection /opt/markov/collector/src/collection/markov_collection
RUN python3 -m pip install --no-deps --no-build-isolation /opt/markov/collector && rm -rf /opt/markov/collector

WORKDIR /workspace/trace-sim

CMD ["bash"]
