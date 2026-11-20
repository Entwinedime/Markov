#!/usr/bin/env bash
set -euo pipefail

cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
sglang_checkout="$(pwd -P)/third_party/sglang"
if ! sglang_root=$(git -C "$sglang_checkout" rev-parse --show-toplevel 2>/dev/null) || [[ "$sglang_root" != "$sglang_checkout" ]]; then
    printf '%s\n' 'Initialize the official SGLang checkout with git submodule update --init third_party/sglang.' >&2
    exit 1
fi

sglang_changes=$(git -C "$sglang_checkout" status --porcelain)
if [[ -n "$sglang_changes" ]]; then
    printf '%s\n' 'Build requires the clean official third_party/sglang checkout.' >&2
    exit 1
fi

# Preserve the actual upstream identity when collector or control code changes.
SGLANG_REVISION=$(git -C "$sglang_checkout" rev-parse HEAD)
export SGLANG_REVISION

# Control must inherit the source image just built, including its native framework metadata.
docker compose -f docker/compose/inference.yml build sglang-profile
docker compose -f docker/compose/inference.yml build control
