FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        clang clang-format cmake g++ ninja-build nlohmann-json3-dev python3 \
    && rm -rf /var/lib/apt/lists/*

ENV CC=clang CXX=clang++
WORKDIR /workspace/markov
CMD ["bash"]
