#!/usr/bin/env bash
set -euo pipefail

MARKOV_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$MARKOV_ROOT"

if [[ "${1:-}" == "--help" ]]; then
    cat <<'HELP'
Usage: scripts/markov.sh plan --project project.json
       scripts/markov.sh predict --project project.json
       scripts/markov.sh explore --project project.json --budget-sec seconds-per-combination
       scripts/markov.sh compare --run run-directory --validation-manifest FILE [--validation-manifest FILE ...]
       scripts/markov.sh prepare-performance --project project.json --prediction-result prediction.json
       scripts/markov.sh profile --project project.json --configuration ID --workload ID
       scripts/markov.sh profile --project project.json --configuration ID --representative
       scripts/markov.sh observe-target --project project.json --configuration ID --workload ID --budget-sec seconds [--purpose ports|structure]
       scripts/markov.sh sample --project project.json --configuration ID --workload ID --kind transfer|device --measurement-requests work-domains.json [--repetitions N]
       scripts/markov.sh sample --project project.json --configuration ID --kind communication [--repetitions N]
       scripts/markov.sh status --run run-directory
       scripts/markov.sh resume --run run-directory [--cost-model FILE ...]
       scripts/markov.sh extract --base-manifest profile_manifest.json
       scripts/markov.sh compile-templates --base-manifest profile_manifest.json
       scripts/markov.sh fit-costs --performance-manifest performance_manifest.json [--unit all|kernel|communication|transfer|command]
       scripts/markov.sh evaluate-costs --model-candidate candidate.json --performance-manifest held-out-manifest.json

plan describes configuration/workload combinations without collecting. predict
uses declared compiled templates and explicit cost candidates or source-bound givens. Representative
collection runs once per missing-source group when its base budget permits.
Rule/cost gaps never trigger structure capture. Actual cost requirements form a
shared sampling plan; permitted measurements reuse the cumulative performance budget.
Cost coverage is checked per instantiated target; missing data remains explicit.
profile --representative executes the candidate recipe within the base budget.
profile and sample are explicit diagnostic acquisition commands; observe-target
produces validation-only facts, using direct return ports by default. --purpose structure
retains full profiler/semantic observations. extract rebuilds compiled templates from observations.
compile-templates reuses existing source graphs and instances for binding changes.
status/resume share the task's cumulative budgets. resume --template-assets FILE
replaces the explicit template list; paths in that JSON array are relative to FILE.
Transfer/device samples require explicit nonempty measurement requests.
fit-costs produces kernel, physical communication, memory-transfer or device-command candidates.
--unit all fits every unit declared by one independent measurement manifest.
Explicit cost candidates can enter conditional prediction under their declared query policies.
All candidates preserve measurement gaps and do not establish cost qualification.
evaluate-costs compares a fixed candidate with separate unit samples without fitting.

HELP
    exit 0
fi

case "${1:-}" in
    observe-target|predict|explore|compare|profile|sample|resume|status)
        export PYTHONPATH="${MARKOV_ROOT}/src/collection${PYTHONPATH:+:${PYTHONPATH}}"
        exec python3 -m markov_collection.workflow.cli "$@"
        ;;
esac

exec docker compose -p markov-next-redesign -f docker/compose/inference.yml \
    run --rm --no-deps modeling build/markov/markov "$@"
