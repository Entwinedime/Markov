#!/usr/bin/env bash
set -euo pipefail

cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
if [[ -n "$(git -C third_party/sglang status --porcelain)" ]]; then
    printf '%s\n' 'Build requires the clean official third_party/sglang checkout.' >&2
    exit 1
fi

# Preserve the actual upstream identity when collector or control code changes.
SGLANG_REVISION=$(git -C third_party/sglang rev-parse HEAD)
export SGLANG_REVISION

# Control must inherit the source image just built, including its native framework metadata.
docker compose -f docker/compose/inference.yml build sglang-profile
docker compose -f docker/compose/inference.yml build control
