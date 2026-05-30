FROM markov-trace-sim-sglang-env:ubuntu22.04

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

COPY scripts/frameworks/sglang/install_from_source.sh /opt/trace-sim/scripts/frameworks/sglang/install_from_source.sh
COPY third_party/sglang /opt/trace-sim/third_party/sglang

RUN chmod +x /opt/trace-sim/scripts/frameworks/sglang/install_from_source.sh \
    && /opt/trace-sim/scripts/frameworks/sglang/install_from_source.sh /opt/trace-sim

WORKDIR /workspace/trace-sim

CMD ["bash"]
