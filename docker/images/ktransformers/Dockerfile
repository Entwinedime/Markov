FROM markov-trace-sim-ktransformers-env:ubuntu22.04

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

COPY scripts/frameworks/ktransformers/install_from_source.sh /opt/trace-sim/scripts/frameworks/ktransformers/install_from_source.sh
COPY third_party/ktransformers /opt/trace-sim/third_party/ktransformers
COPY third_party/sglang /opt/trace-sim/third_party/sglang

RUN chmod +x /opt/trace-sim/scripts/frameworks/ktransformers/install_from_source.sh \
    && /opt/trace-sim/scripts/frameworks/ktransformers/install_from_source.sh /opt/trace-sim

WORKDIR /workspace/trace-sim

CMD ["bash"]
