"""Materialize bounded real-data replays and a controlled cache-reuse workload."""

from __future__ import annotations

import argparse
from collections import Counter, deque
import csv
import json
from pathlib import Path
import random
import statistics

from examples.hicache_matrix.inputs import conversation_identity, request
from markov.artifacts.writing import write_json


SEED = 20261109


def sharegpt(dataset: Path, tokenizer, excluded: set[str]) -> tuple[list[dict], list[dict], dict]:
    records = json.loads(dataset.read_text())
    random.Random(SEED).shuffle(records)
    conversations = []
    seen = set(excluded)
    skipped = Counter()
    for record in records:
        identity = conversation_identity(str(record["id"]))
        if identity in seen:
            skipped["duplicate_or_previous_identity"] += 1
            continue
        seen.add(identity)
        messages, turns = [], []
        for index in range(0, len(record["conversations"]) - 1, 2):
            user, answer = record["conversations"][index : index + 2]
            if (
                user.get("from") != "human"
                or answer.get("from") != "gpt"
                or not isinstance(user.get("value"), str)
                or not isinstance(answer.get("value"), str)
                or not user["value"].strip()
                or not answer["value"].strip()
            ):
                skipped["invalid_turn"] += 1
                break
            messages.append({"role": "user", "content": user["value"]})
            tokens = tokenizer.apply_chat_template(
                messages, tokenize=True, return_dict=False, add_generation_prompt=True, enable_thinking=False
            )
            output = len(tokenizer.encode(answer["value"], add_special_tokens=False))
            if len(tokens) > 2048:
                skipped["input_above_limit"] += 1
                break
            if len(tokens) >= 256 and output > 0:
                turns.append((tokens, output, {"identity": identity, "record_id": record["id"], "turn": index // 2}))
            else:
                skipped["input_below_limit_or_empty_answer"] += 1
            messages.append({"role": "assistant", "content": answer["value"]})
        if len(turns) >= 2:
            conversations.append(turns)
        if sum(len(rows) for rows in conversations) >= 1024:
            break

    active = deque(deque(rows) for rows in conversations[:32])
    remaining = iter(conversations[32:])
    selected = []
    while active and len(selected) < 512:
        turns = active.popleft()
        selected.append(turns.popleft())
        if not turns:
            replacement = next(remaining, None)
            if replacement is not None:
                active.append(deque(replacement))
        else:
            active.append(turns)
    if len(selected) != 512:
        raise ValueError(f"ShareGPT yielded {len(selected)} eligible requests; need 512")
    rows = [request(tokens, max(1, output)) for tokens, output, _ in selected]
    origins = [{**origin, "original_output_tokens": output} for _, output, origin in selected]
    return rows, origins, {"skipped": dict(skipped), "active_conversations": 32, "scanned_records": len(records)}


def burstgpt(dataset: Path, rng: random.Random, safe_tokens: list[int]) -> tuple[list[dict], list[dict], dict]:
    rows, origins = [], []
    skipped = Counter()
    with dataset.open(newline="") as stream:
        for line, record in enumerate(csv.DictReader(stream), 2):
            length, output = int(record["Request tokens"]), int(record["Response tokens"])
            if record["Model"] != "ChatGPT" or record["Log Type"] != "API log":
                skipped["model_or_log_type"] += 1
                continue
            if not 256 <= length <= 2048 or output <= 0:
                skipped["length_or_failed_response"] += 1
                continue
            tokens = rng.choices(safe_tokens, k=length)
            rows.append(request(tokens, output))
            origins.append({"line": line, "record": record, "original_output_tokens": output})
            if len(rows) == 256:
                break
    if len(rows) != 256:
        raise ValueError(f"BurstGPT yielded {len(rows)} eligible requests; need 256")
    return rows, origins, {"skipped": dict(skipped), "content": "Independent synthetic tokens; lengths from API logs"}


def generated(rng: random.Random, safe_tokens: list[int]) -> tuple[list[dict], list[dict], dict]:
    families = [rng.choices(safe_tokens, k=768 if index % 2 == 0 else 1536) for index in range(32)]
    rows, origins = [], []
    last = {}
    for block in range(12):
        schedule = [("hot", index) for index in range(32)]
        schedule += [("revisit", index) for index in range(25)]
        schedule += [("hot", rng.randrange(8)) for _ in range(28)]
        schedule += [("new", None)] * 15
        for kind, family in schedule:
            index = len(rows)
            prefix = (
                families[family] if family is not None else rng.choices(safe_tokens, k=768 if index % 2 == 0 else 1536)
            )
            distance = index - last[family] if family in last else None
            if kind == "revisit" and (distance is None or not 16 <= distance <= 64):
                raise ValueError("Generated revisit violates the declared distance")
            suffix = rng.choices(safe_tokens, k=rng.randint(64, 256))
            rows.append(request(prefix + suffix, 8))
            origins.append({"kind": kind, "family": family, "block": block, "reuse_distance": distance})
            if family is not None:
                last[family] = index
    if Counter(row["kind"] for row in origins) != {"hot": 720, "revisit": 300, "new": 180}:
        raise ValueError("Generated request counts differ from the declared protocol")
    return rows, origins, {"reusable_families": 32, "new_prefixes": 180}


def summarize(rows: list[dict], original: list[int], cap: int | None) -> dict:
    lengths = [len(row["input_ids"]) for row in rows]
    outputs = [row["max_new_tokens"] for row in rows]
    clipped = sum(value > cap for value in original) if cap is not None else 0
    prefixes = {}
    reused = []
    for row in rows:
        node, common = prefixes, 0
        for token in row["input_ids"]:
            if token not in node:
                break
            node = node[token]
            common += 1
        reused.append(common)
        node = prefixes
        for token in row["input_ids"]:
            node = node.setdefault(token, {})
    return {
        "formal_requests": len(rows),
        "input_tokens": sum(lengths),
        "output_tokens": sum(outputs),
        "input_min": min(lengths),
        "input_median": statistics.median(lengths),
        "input_max": max(lengths),
        "original_output_tokens": sum(original),
        "original_output_min": min(original),
        "original_output_median": statistics.median(original),
        "original_output_max": max(original),
        "output_min": min(outputs),
        "output_median": statistics.median(outputs),
        "output_max": max(outputs),
        "clipped_requests": clipped,
        "clipped_request_ratio": clipped / len(rows),
        "discarded_output_token_ratio": 1 - sum(outputs) / sum(original),
        "input_lengths": lengths,
        "original_output_lengths": original,
        "output_lengths": outputs,
        "potential_reused_prefix_tokens": reused,
        "potential_prefix_reuse_ratio": sum(reused) / sum(lengths),
        "prefix_qualification": "Maximum prefix shared with earlier input; ignores capacity, residency and generated outputs",
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--sharegpt", type=Path, required=True)
    parser.add_argument("--burstgpt", type=Path, required=True)
    parser.add_argument("--exclude", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--output-cap", type=int, choices=(16, 32), default=32)
    args = parser.parse_args()

    from transformers import AutoTokenizer

    tokenizer = AutoTokenizer.from_pretrained(args.model, local_files_only=True)
    safe_tokens = sorted(set(tokenizer.get_vocab().values()) - set(tokenizer.all_special_ids))
    excluded = json.loads(args.exclude.read_text())
    identities = set(excluded["excluded_identities"]) | set(excluded["new_identities"])
    args.output.mkdir(parents=True, exist_ok=True)
    warm_rng = random.Random(SEED - 1)
    warm = [
        request(warm_rng.choices(safe_tokens, k=length), 8) for length in [256, 512, 768, 1024, 1536, 2048, 512, 1024]
    ]
    sources = {
        "sharegpt": sharegpt(args.sharegpt, tokenizer, identities),
        "burstgpt": burstgpt(args.burstgpt, random.Random(SEED), safe_tokens),
        "generated": generated(random.Random(SEED + 1), safe_tokens),
    }
    selection = {
        "seed": SEED,
        "output_cap": args.output_cap,
        "excluded_identities": sorted(identities),
        "sources": {
            "sharegpt": "https://huggingface.co/datasets/anon8231489123/ShareGPT_Vicuna_unfiltered/resolve/main/ShareGPT_V3_unfiltered_cleaned_split.json",
            "sharegpt_prefix_records": 16384,
            "burstgpt": "https://raw.githubusercontent.com/HPMLL/BurstGPT/main/data/BurstGPT_1.csv",
            "burstgpt_model": "ChatGPT",
            "burstgpt_log_type": "API log",
        },
        "workloads": {},
    }
    for name, (rows, origins, metadata) in sources.items():
        original = [row["max_new_tokens"] for row in rows]
        if name != "generated":
            rows = [{**row, "max_new_tokens": min(args.output_cap, row["max_new_tokens"])} for row in rows]
        payload = {
            "arrival": "serial",
            "formal_begin_index": len(warm),
            "drain_before_formal": True,
            "requests": warm + rows,
        }
        write_json(args.output / f"{name}.json", payload)
        write_json(args.output / f"{name}_pilot.json", {**payload, "requests": warm + rows[:20]})
        selection["workloads"][name] = {
            **metadata,
            **summarize(rows, original, args.output_cap if name != "generated" else None),
            "origins": origins,
        }
        print(
            name,
            len(rows),
            "input",
            sum(len(row["input_ids"]) for row in rows),
            "output",
            sum(row["max_new_tokens"] for row in rows),
            flush=True,
        )
    write_json(args.output / "selection.json", selection)


if __name__ == "__main__":
    main()
