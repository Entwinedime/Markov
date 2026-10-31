"""Materialize declared benchmark inputs on CPU, independently of target results."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import random
from typing import TextIO

from markov.artifacts.request import read_json
from markov.artifacts.writing import write_json


@dataclass(frozen=True)
class Conversation:
    identity: str
    record_id: str
    prompts: list[list[int]]


def conversation_prefix(stream: TextIO, limit: int) -> list[dict]:
    """Read a bounded prefix of the public JSON array without loading the corpus."""
    decoder = json.JSONDecoder()
    buffer, rows, opened = "", [], False
    while len(rows) < limit:
        chunk = stream.read(1024 * 1024)
        buffer += chunk
        if not opened:
            buffer = buffer.lstrip()
            if not buffer.startswith("["):
                raise ValueError("ShareGPT input must be a JSON array")
            buffer, opened = buffer[1:], True
        while len(rows) < limit:
            buffer = buffer.lstrip().removeprefix(",").lstrip()
            if buffer.startswith("]"):
                return rows
            try:
                record, end = decoder.raw_decode(buffer)
            except json.JSONDecodeError:
                if not chunk:
                    raise
                break
            rows.append(record)
            buffer = buffer[end:]
    return rows


def request(tokens: list[int], output_tokens: int) -> dict:
    return {"input_ids": tokens, "max_new_tokens": output_tokens, "ignore_eos": True}


def manual_inputs(specifications: dict, tokenizer, output: Path) -> None:
    for name, specification in specifications.items():
        prompts = {}
        for family in specification["families"]:
            for index, branch in enumerate(family["branches"]):
                parts = []
                for part in family["parts"]:
                    fragment = specification["fragments"][part["ref"]] if "ref" in part else part
                    parts.append(fragment["text"].format(branch=branch) * fragment.get("repeat", 1))
                tokens = tokenizer.encode("".join(parts))
                if len(tokens) != family["tokens"]:
                    raise ValueError(f"{name}/{family['name']}: tokenizer differs from the declared token contract")
                prompts[family["name"].format(i=index)] = tokens

        rows = [
            request(prompts[prompt], specification["output_tokens"])
            for prompt, count in specification["prepare"]
            for _ in range(count)
        ]
        formal_begin = len(rows)
        rows.extend(request(prompts[prompt], specification["output_tokens"]) for prompt in specification["formal"])
        write_json(
            output / f"{name}.json",
            {"requests": rows, "arrival": "serial", "formal_begin_index": formal_begin, "drain_before_formal": True},
        )


def conversations(dataset: Path, policy: dict, tokenizer) -> list[Conversation]:
    with dataset.open(encoding="utf-8") as stream:
        records = conversation_prefix(stream, policy["scan_conversations"])
    random.Random(policy["seed"]).shuffle(records)
    result, seen = [], set()
    for record in records:
        identity, separator, suffix = str(record["id"]).rpartition("_")
        identity = identity if separator and suffix.isdigit() else str(record["id"])
        if identity in seen:
            continue
        seen.add(identity)

        messages, prompts = [], []
        for turn in record["conversations"]:
            role = {"human": "user", "gpt": "assistant"}.get(turn.get("from"))
            text = turn.get("value")
            if (
                role != ("user" if len(messages) % 2 == 0 else "assistant")
                or not isinstance(text, str)
                or not text.strip()
            ):
                break
            messages.append({"role": role, "content": text})
            if role == "user":
                tokens = tokenizer.apply_chat_template(
                    messages, tokenize=True, return_dict=False, add_generation_prompt=True, enable_thinking=False
                )
                if not policy["minimum_input_tokens"] <= len(tokens) <= policy["maximum_input_tokens"]:
                    break
                prompts.append(tokens)
        if prompts:
            result.append(Conversation(identity, str(record["id"]), prompts))
    return result


def take(candidates: list[Conversation], count: int, used: set[str]) -> list[Conversation]:
    chosen = [item for item in candidates if item.identity not in used][:count]
    if len(chosen) != count:
        raise ValueError(f"ShareGPT selection needs {count} eligible conversations, found {len(chosen)}")
    used.update(item.identity for item in chosen)
    return chosen


def sharegpt_inputs(dataset: Path, policy: dict, tokenizer, output: Path) -> None:
    pool = conversations(dataset, policy, tokenizer)
    used: set[str] = set()
    selections = {}
    for split in ("development", "confirmation"):
        settings = policy[split]
        candidates = list(pool)
        random.Random(settings["seed"]).shuffle(candidates)
        independent = []
        boundaries = policy["independent_length_boundaries"]
        for lower, upper in zip(boundaries, boundaries[1:]):
            independent.extend(
                take(
                    [item for item in candidates if lower <= len(item.prompts[0]) < upper],
                    settings["independent_requests_per_bucket"],
                    used,
                )
            )
        turns = settings["multi_turn_turns"]
        multi = take(
            [item for item in candidates if len(item.prompts) >= turns], settings["multi_turn_conversations"], used
        )
        revisit = take(
            [item for item in candidates if len(item.prompts[-1]) >= policy["revisit_minimum_tokens"]],
            settings["revisit_conversations"],
            used,
        )
        warmup = take(candidates, 1, used)[0]
        groups = {
            "independent": [(item, 0) for item in independent],
            "multi_turn": [(item, turn) for turn in range(turns) for item in multi],
            "revisit": [(item, len(item.prompts) - 1) for _ in range(settings["revisit_passes"]) for item in revisit],
        }
        selections[split] = {"warmup": warmup.record_id, "conversations": {}}
        for kind, selected in groups.items():
            name = ("confirmation_" if split == "confirmation" else "") + f"sharegpt_{kind}"
            output_tokens = policy["output_tokens"][kind]
            write_json(
                output / f"{name}.json",
                {
                    "arrival": "serial",
                    "formal_begin_index": 1,
                    "requests": [request(warmup.prompts[0], output_tokens)]
                    + [request(item.prompts[turn], output_tokens) for item, turn in selected],
                },
            )
            selections[split]["conversations"][kind] = list(dict.fromkeys(item.record_id for item, _ in selected))
    write_json(output / "sharegpt_selection.json", {"policy": policy, "selections": selections})


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--recipe", type=Path, required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--dataset", type=Path)
    parser.add_argument("--sets", nargs="+", required=True)
    args = parser.parse_args()
    recipe = read_json(args.recipe)
    args.output.mkdir(parents=True, exist_ok=True)

    # Only the CPU input builder needs Transformers; prediction does not.
    from transformers import AutoTokenizer

    tokenizer = AutoTokenizer.from_pretrained(args.model, local_files_only=True)
    if "manual" in args.sets:
        manual_inputs(read_json(Path(__file__).with_name("manual.json")), tokenizer, args.output)
    if set(args.sets) & {"development", "confirmation"}:
        policy = {
            **recipe["sharegpt"],
            "dataset_url": recipe["dataset_url"],
            "scan_rule": "Read the first scan_conversations records, then shuffle by the declared seed.",
        }
        sharegpt_inputs(args.dataset, policy, tokenizer, args.output)


if __name__ == "__main__":
    main()
