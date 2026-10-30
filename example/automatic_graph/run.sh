#!/usr/bin/env bash
set -euo pipefail

MARKOV_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
MARKOV_DOCKER="$(command -v docker)"
MARKOV_COMPOSE="$(docker info --format '{{range .ClientInfo.Plugins}}{{if eq .Name "compose"}}{{.Path}}{{end}}{{end}}')"
MARKOV_IMAGE_PREFIX="${MARKOV_IMAGE_PREFIX:-markov-next}"
MARKOV_ENTRY=(-m markov_application.exploration)
if [[ "${1:-}" == "validate" ]]; then
    shift
    MARKOV_ENTRY=("$MARKOV_ROOT/example/automatic_graph/validation.py")
elif [[ "${1:-}" == "matrix" ]]; then
    shift
    MARKOV_ENTRY=("$MARKOV_ROOT/example/automatic_graph/matrix.py")
elif [[ "${1:-}" == "investigate" ]]; then
    shift
    MARKOV_ENTRY=("$MARKOV_ROOT/example/automatic_graph/investigation.py")
fi

# Keep Python and simulation in the modeling environment; the mounted CLI controls only this run's containers.
exec docker run --rm --init --network none \
    -v "$MARKOV_ROOT:$MARKOV_ROOT" \
    -v /var/run/docker.sock:/var/run/docker.sock \
    -v "$MARKOV_DOCKER:/usr/bin/docker:ro" \
    -v "$MARKOV_COMPOSE:/usr/libexec/docker/cli-plugins/docker-compose:ro" \
    -e "MARKOV_IMAGE_PREFIX=$MARKOV_IMAGE_PREFIX" \
    -e "PYTHONPATH=$MARKOV_ROOT/src/application:$MARKOV_ROOT/src/collection:$MARKOV_ROOT/src/workload" \
    -w "$MARKOV_ROOT" "$MARKOV_IMAGE_PREFIX-modeling:ubuntu24.04" \
    python3 "${MARKOV_ENTRY[@]}" "$@"
