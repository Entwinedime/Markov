#!/usr/bin/env bash
set -euo pipefail

MARKOV_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$MARKOV_ROOT"

model=""
devices=""
resume=""
image_prefix="${MARKOV_IMAGE_PREFIX:-markov-next}"

while (($#)); do
    case "$1" in
        --help|-h)
            cat <<'HELP'
Usage: example/sglang_qwen3/run.sh --model /models/Qwen3-32B --devices 4,5 [--image-prefix PREFIX]
       example/sglang_qwen3/run.sh --resume data/RUN [--image-prefix PREFIX]

Runs the declared two-configuration example from empty assets, then observes
independent target ports. Read results/summary.md in the printed run directory.
Images and model weights must already be installed; see the adjacent README.
HELP
            exit 0
            ;;
        --model|--devices|--resume|--image-prefix)
            if (($# < 2)); then
                printf 'Missing value for %s\n' "$1" >&2
                exit 2
            fi
            case "$1" in
                --model) model="$2" ;;
                --devices) devices="$2" ;;
                --resume) resume="$2" ;;
                --image-prefix) image_prefix="$2" ;;
            esac
            shift 2
            ;;
        *) printf 'Unknown argument: %s\n' "$1" >&2; exit 2 ;;
    esac
done

export MARKOV_IMAGE_PREFIX="$image_prefix"
docker info >/dev/null

if [[ -n "$resume" ]]; then
    if [[ -n "$model" || -n "$devices" ]]; then
        printf '%s\n' '--resume uses the recorded model and devices; omit --model and --devices.' >&2
        exit 2
    fi
    exec scripts/markov.sh resume --run "$resume"
fi

if [[ -z "$model" || -z "$devices" ]]; then
    printf '%s\n' 'Specify --model and --devices; use --help for usage.' >&2
    exit 2
fi
if [[ ! -r "$model/config.json" ]]; then
    printf 'Model configuration is not readable: %s/config.json\n' "$model" >&2
    exit 2
fi

scripts/build.sh
exec scripts/markov.sh explore --project example/sglang_qwen3/project.json \
    --model "$model" --devices "$devices" --budget-sec 300
