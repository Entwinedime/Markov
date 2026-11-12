ARG BASE_IMAGE=markov-next-sglang-env:ubuntu22.04
FROM ${BASE_IMAGE}

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

ARG SGLANG_REVISION
RUN [[ "$SGLANG_REVISION" =~ ^[0-9a-f]{40}$ ]]
ENV MARKOV_SGLANG_REVISION=${SGLANG_REVISION}
LABEL org.markov.sglang.revision=${SGLANG_REVISION}

COPY src/collection/native/frameworks/sglang/ascend/install.sh /opt/markov/install-sglang.sh
COPY third_party/sglang /opt/markov/sglang

RUN bash /opt/markov/install-sglang.sh /opt/markov/sglang && rm /opt/markov/install-sglang.sh

# FA keeps the paged KV layout consumed by native mixed-batch attention.
ENV ASCEND_USE_FA=true ASCEND_USE_FIA=false

# The SDK dumper must publish its running flag before starting the background thread.
COPY src/collection/native/runtimes/ascend /opt/markov/native/ascend
RUN bash /opt/markov/native/ascend/profiler/install.sh

RUN source /usr/local/Ascend/ascend-toolkit/set_env.sh \
    && cmake -S /opt/markov/native/ascend -B /tmp/markov-ascend -DCMAKE_BUILD_TYPE=Release \
        -DMARKOV_ASCEND_INCLUDE_DIR=/usr/local/Ascend/ascend-toolkit/latest/include \
    && cmake --build /tmp/markov-ascend --parallel \
    && mkdir -p /opt/markov/lib \
    && cp /tmp/markov-ascend/libmarkov_ascend_observer.so /opt/markov/lib/ \
    && rm -rf /tmp/markov-ascend

ENV LD_PRELOAD=/opt/markov/lib/libmarkov_ascend_observer.so

COPY --chmod=755 src/collection/markov_collection/adapters/runtimes/ascend/export.sh /opt/markov/export-profile.sh
COPY --chmod=755 src/collection/markov_collection/adapters/runtimes/ascend/capture.sh /opt/markov/capture.sh
COPY pyproject.toml /opt/markov/collector/pyproject.toml
COPY src/collection/markov_collection /opt/markov/collector/src/collection/markov_collection
COPY src/workload/markov_workload /opt/markov/collector/src/workload/markov_workload
COPY src/adapters/markov_adapters /opt/markov/collector/src/adapters/markov_adapters
RUN python3 -m pip install --no-deps --no-build-isolation /opt/markov/collector && rm -rf /opt/markov/collector

WORKDIR /workspace/trace-sim

CMD ["bash"]
