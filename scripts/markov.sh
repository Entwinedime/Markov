#!/usr/bin/env bash
set -euo pipefail

MARKOV_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$MARKOV_ROOT"

if [[ "${1:-}" == "--help" ]]; then
    cat <<'HELP'
Usage: scripts/markov.sh plan --project project.json
       scripts/markov.sh predict --project project.json
       scripts/markov.sh profile --project project.json --configuration ID --workload ID
       scripts/markov.sh profile --project project.json --configuration ID --representative
       scripts/markov.sh observe-target --project project.json --configuration ID --workload ID --budget-sec seconds
       scripts/markov.sh sample --project project.json --configuration ID --workload ID --kind cpu|device --measurement-requests work-domains.json [--repetitions N]
       scripts/markov.sh sample --project project.json --configuration ID --kind communication [--repetitions N]
       scripts/markov.sh status --run run-directory
       scripts/markov.sh resume --run run-directory
       scripts/markov.sh extract --base-manifest profile_manifest.json
       scripts/markov.sh compile-templates --base-manifest profile_manifest.json

plan describes configuration/workload combinations without collecting. predict
uses declared compiled templates and source-bound node durations. Representative
collection reports candidate mechanisms; automatic collection and real node cost
models remain unimplemented.
profile --representative executes the candidate recipe within the base budget.
profile and sample are explicit diagnostic acquisition commands; observe-target
produces validation-only facts. extract rebuilds compiled templates from observations.
compile-templates reuses existing source graphs and instances for binding changes.
status/resume share the task's cumulative budgets. resume --template-assets FILE
replaces the explicit template list; paths in that JSON array are relative to FILE.
CPU/device samples require explicit nonempty measurement requests.

HELP
    exit 0
fi

if [[ "${1:-}" == "observe-target" || "${1:-}" == "predict" || "${1:-}" == "profile" || "${1:-}" == "sample" || "${1:-}" == "resume" || "${1:-}" == "status" ]]; then
    export PYTHONPATH="${MARKOV_ROOT}/src/collection${PYTHONPATH:+:${PYTHONPATH}}"
    exec python3 -m markov_collection.workflow.collection "$@"
fi

exec docker compose -p markov-next-redesign -f docker/compose/inference.yml \
    run --rm --no-deps modeling build/markov/markov "$@"
