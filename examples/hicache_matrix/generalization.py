"""Declare unseen configuration combinations and bounded, disjoint ShareGPT requests."""

from __future__ import annotations

import argparse
from itertools import combinations, product
import json
from pathlib import Path
import random

from markov.artifacts.request import read_json
from markov.artifacts.writing import write_json

from inputs import Conversation, conversation_identity, conversations, request, take


SEED = 20261102
WORKLOADS = ["independent_short", "independent_long", "multi_turn", "revisit"]
AXES = {
    "page_size": [32, 64, 128],
    "max_total_tokens": [3072, 4096, 6144, 8192],
    "hicache_ratio": [1.5, 2.0, 4.0],
    "hicache_write_policy": ["write_through", "write_through_selective", "write_back"],
}


def canonical(configuration: dict) -> str:
    fields = dict(configuration)
    fields["hicache_storage_backend_extra_config"] = json.loads(fields["hicache_storage_backend_extra_config"])
    return json.dumps(fields, sort_keys=True)


def parameter_pairs(configuration: dict) -> set[tuple]:
    fields = {name: configuration[name] for name in AXES}
    fields["prefetch_policy"] = configuration["hicache_storage_prefetch_policy"]
    extra = json.loads(configuration["hicache_storage_backend_extra_config"])
    fields["prefetch_threshold"] = extra["prefetch_threshold"]
    if fields["prefetch_policy"] == "timeout":
        fields["timeout_sec"] = extra["prefetch_timeout_base"]
    return set(combinations(sorted(fields.items()), 2))


def configuration_panel(reference: dict) -> tuple[dict, dict]:
    historical = {canonical(row) for row in reference["configurations"].values()}
    candidates = []
    for values in product(*AXES.values()):
        for policy, timeout in [
            ("wait_complete", None),
            ("best_effort", None),
            *[("timeout", t) for t in (0.01, 0.1, 1.0)],
        ]:
            for threshold in (128, 512, 1024):
                extra = {"prefetch_threshold": threshold}
                if timeout is not None:
                    extra.update(
                        prefetch_timeout_base=timeout, prefetch_timeout_per_ki_token=0.0, prefetch_timeout_max=timeout
                    )
                row = {
                    **dict(zip(AXES, values)),
                    "hicache_storage_prefetch_policy": policy,
                    "hicache_storage_backend_extra_config": json.dumps(extra, sort_keys=True),
                }
                if canonical(row) not in historical:
                    candidates.append(row)

    candidates.sort(key=canonical)
    pair_sets = [parameter_pairs(row) for row in candidates]
    universe = set().union(*pair_sets)
    selected = {name: reference["configurations"][name] for name in ("C2", "C5")}
    covered = set().union(*(parameter_pairs(row) for row in selected.values()))
    remaining = list(range(len(candidates)))
    prefixes = []
    for number in range(1, 23):
        # Remaining order provides the declared configuration-content tie break.
        winner = max(remaining, key=lambda i: len(pair_sets[i] - covered))
        remaining.remove(winner)
        selected[f"candidate_{number:02d}"] = candidates[winner]
        covered.update(pair_sets[winner])
        prefixes.append({"configurations": len(selected), "covered_pairs": len(covered & universe)})
    uncovered = sorted(universe - covered)
    return selected, {
        "candidate_count": len(candidates),
        "candidate_pairs": len(universe),
        "covered_pairs": len(covered & universe),
        "uncovered_pairs": uncovered,
        "prefix_coverage": prefixes,
        "selection": "Greedy unseen parameter-pair coverage, then canonical configuration-content order; no predictions or scores.",
    }


def excluded_conversations(paths: list[Path]) -> set[str]:
    excluded = set()
    for path in paths:
        for split in read_json(path)["selections"].values():
            excluded.add(conversation_identity(split["warmup"]))
            for records in split["conversations"].values():
                excluded.update(conversation_identity(record) for record in records)
    return excluded


def workload_inputs(pool: list[Conversation], excluded: set[str], output: Path) -> dict:
    used = set(excluded)
    independent = []
    for lower, upper in [(128, 512), (512, 1536), (1536, 2049)]:
        independent.extend(take([row for row in pool if lower <= len(row.prompts[0]) < upper], 4, used))
    multi = take([row for row in pool if len(row.prompts) >= 4], 3, used)
    revisit_turns = {
        row.identity: max(i for i, tokens in enumerate(row.prompts) if 1024 <= len(tokens) <= 2048)
        for row in pool
        if any(1024 <= len(tokens) <= 2048 for tokens in row.prompts)
    }
    revisit = take([row for row in pool if row.identity in revisit_turns], 6, used)
    random.Random(SEED).shuffle(revisit)
    warmups = take([row for row in pool if 128 <= len(row.prompts[0]) < 512], 3, used)
    selections = {
        "independent_short": [(row, 0) for row in independent],
        "independent_long": [(row, 0) for row in independent],
        "multi_turn": [(row, turn) for turn in range(4) for row in multi],
        "revisit": [(row, revisit_turns[row.identity]) for _ in range(2) for row in revisit],
    }
    warmup_by_workload = {
        "independent_short": warmups[0],
        "independent_long": warmups[0],
        "multi_turn": warmups[1],
        "revisit": warmups[2],
    }
    details = {}
    for name, rows in selections.items():
        warmup = warmup_by_workload[name]
        output_tokens = 32 if name == "independent_long" else 8
        requests = [request(warmup.prompts[0], output_tokens)] + [
            request(row.prompts[turn], output_tokens) for row, turn in rows
        ]
        write_json(
            output / f"{name}.json",
            {"arrival": "serial", "formal_begin_index": 1, "drain_before_formal": True, "requests": requests},
        )
        details[name] = {
            "warmup": warmup.record_id,
            "conversations": list(dict.fromkeys(row.record_id for row, _ in rows)),
            "request_turns": [{"record_id": row.record_id, "turn": turn} for row, turn in rows],
            "formal_requests": len(rows),
            "input_tokens_including_warmup": sum(len(row["input_ids"]) for row in requests),
            "output_tokens_including_warmup": sum(row["max_new_tokens"] for row in requests),
        }
    return {
        "workloads": details,
        "excluded_identities": sorted(excluded),
        "selected_new_identities": sorted(used - excluded),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference-recipe", type=Path, default=Path(__file__).with_name("recipe.json"))
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--dataset", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--exclude-selection", type=Path, nargs="+", required=True)
    args = parser.parse_args()
    reference = read_json(args.reference_recipe)
    configurations, coverage = configuration_panel(reference)

    # Tokenization is confined to this CPU experiment-input producer.
    from transformers import AutoTokenizer

    tokenizer = AutoTokenizer.from_pretrained(args.model, local_files_only=True)
    policy = {**reference["sharegpt"], "seed": SEED, "minimum_input_tokens": 1, "maximum_input_tokens": 2304}
    pool = conversations(args.dataset, policy, tokenizer)
    inputs = args.output / "inputs"
    inputs.mkdir(parents=True, exist_ok=True)
    selection = workload_inputs(pool, excluded_conversations(args.exclude_selection), inputs)
    write_json(
        inputs / "selection.json", {"seed": SEED, "scan_conversations": policy["scan_conversations"], **selection}
    )
    recipe = {
        **reference,
        "configurations": configurations,
        "configuration_sets": {"validation": list(configurations)},
        "workload_sets": {"validation": WORKLOADS},
        "panels": {
            "generalization": {
                "expected_cross": 184,
                "groups": [{"bases": ["C2", "C5"], "workloads": "validation", "targets": "validation"}],
            }
        },
        "measurement_seed": SEED,
        "source_budget": {"starts": 2, "wall_sec": 900, "retained_mib": 8192},
        "normal_budget": {"starts": 2, "wall_sec": 600, "retained_mib": 32768},
    }
    write_json(args.output / "recipe.json", recipe)
    write_json(args.output / "coverage.json", coverage)
    print(
        json.dumps(
            {
                "configurations": len(configurations),
                "workloads": WORKLOADS,
                "new_base_profiles": 8,
                "coverage": coverage["covered_pairs"],
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
