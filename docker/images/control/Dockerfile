ARG BASE_IMAGE=markov-next-sglang:ubuntu22.04
FROM ${BASE_IMAGE}

ENV LD_PRELOAD=""

# Native control uses CPU metadata; accelerator discovery and observation plugins are not part of prediction.
RUN python3 -m pip uninstall -y torch-npu flash-attn-npu markov-collection

COPY src/control/markov_control /opt/markov/control/markov_control
COPY src/workload/markov_workload /opt/markov/control/markov_workload
COPY src/adapters/markov_adapters /opt/markov/control/markov_adapters
ENV PYTHONPATH=/opt/markov/control
ENV SGLANG_USE_CPU_ENGINE=1
ENV OMP_NUM_THREADS=1

ENTRYPOINT ["python3", "-m", "markov_control"]
