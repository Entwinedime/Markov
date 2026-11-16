#!/usr/bin/env bash
set -e

source "$KTRANSFORMERS_REPO/kt-kernel/tools/ascend_dsv4/dsv4_env.sh"

exec "$@"
