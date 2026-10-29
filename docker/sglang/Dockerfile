ARG SGLANG_RUNTIME_IMAGE
FROM ${SGLANG_RUNTIME_IMAGE}

# Use a declared, initialized inference environment; modeling never loads it.
COPY pyproject.toml /opt/markov/pyproject.toml
COPY python /opt/markov/python
RUN python3 -m pip install --no-index --no-deps --no-build-isolation /opt/markov
