#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

git submodule update --init --recursive third_party/sglang third_party/ktransformers

git -C third_party/ktransformers submodule update --init --recursive \
    archive/third_party/PhotonLibOS \
    archive/third_party/custom_flashinfer \
    archive/third_party/llama.cpp \
    archive/third_party/prometheus-cpp \
    archive/third_party/pybind11 \
    archive/third_party/spdlog \
    archive/third_party/xxHash
