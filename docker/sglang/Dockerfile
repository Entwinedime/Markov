ARG SGLANG_RUNTIME_IMAGE
FROM ${SGLANG_RUNTIME_IMAGE}

# Use a declared, initialized inference environment; modeling never loads it.
COPY pyproject.toml /opt/markov/pyproject.toml
COPY python /opt/markov/python
RUN python3 -m pip install --no-index --no-deps --no-build-isolation /opt/markov

COPY src/markov/adapters/ascend/profiling /opt/markov/native
RUN cmake -S /opt/markov/native -B /opt/markov/native/build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /opt/markov/native/build --parallel 2 \
    && cmake --install /opt/markov/native/build --prefix /opt/markov
