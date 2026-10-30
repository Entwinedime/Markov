ARG BASE_IMAGE=markov-next-sglang-env:ubuntu22.04
FROM ${BASE_IMAGE}

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

COPY src/collection/native/frameworks/sglang/ascend/install.sh /opt/markov/install-sglang.sh
COPY third_party/sglang /opt/markov/sglang

RUN bash /opt/markov/install-sglang.sh /opt/markov/sglang && rm /opt/markov/install-sglang.sh

# FA keeps the paged KV layout consumed by native mixed-batch attention.
ENV ASCEND_USE_FA=true ASCEND_USE_FIA=false

COPY pyproject.toml /opt/markov/collector/pyproject.toml
COPY src/collection/markov_collection /opt/markov/collector/src/collection/markov_collection
COPY src/workload/markov_workload /opt/markov/collector/src/workload/markov_workload
RUN python3 -m pip install --no-deps --no-build-isolation /opt/markov/collector && rm -rf /opt/markov/collector

WORKDIR /workspace/trace-sim

CMD ["bash"]
