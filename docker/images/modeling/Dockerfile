FROM ubuntu:24.04

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

ARG DEBIAN_FRONTEND=noninteractive
ENV TZ=Etc/UTC

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        bash \
        build-essential \
        ca-certificates \
        ccache \
        clang \
        clang-format \
        clang-tidy \
        cmake \
        curl \
        file \
        g++ \
        gcc \
        git \
        jq \
        less \
        libssl-dev \
        locales \
        lld \
        make \
        ninja-build \
        pkg-config \
        python3 \
        python3-pip \
        python3-venv \
        python3-yaml \
        rsync \
    && rm -rf /var/lib/apt/lists/*

RUN locale-gen en_US.UTF-8

ENV LANG=en_US.UTF-8
ENV LANGUAGE=en_US:en
ENV LC_ALL=en_US.UTF-8
ENV CC=clang
ENV CXX=clang++
ENV CMAKE_GENERATOR=Ninja

WORKDIR /workspace/trace-sim

CMD ["bash"]
