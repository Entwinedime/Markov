#!/usr/bin/env bash

repo_root() {
    cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd
}

compose_file() {
    printf 'docker/compose/inference.yml\n'
}

profile_service() {
    local framework=${1:-}
    case "$framework" in
        sglang|ktransformers)
            printf '%s-profile\n' "$framework"
            ;;
        *)
            echo "unknown framework: ${framework}" >&2
            echo "known frameworks: sglang, ktransformers" >&2
            return 2
            ;;
    esac
}

run_in_container() {
    local framework=$1
    shift

    local service
    service="$(profile_service "$framework")"
    docker compose -f "$(compose_file)" run --rm "$service" "$@"
}
