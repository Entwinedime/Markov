"""Turn explicitly selected ShareGPT conversations into existing serial workloads."""

from __future__ import annotations

import random
import statistics
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from ..common.io import load_json, write_json
from .schema import TemplateValidationError


@dataclass(frozen=True)
class Conversation:
    conversation_id: str
    record_id: str
    prompts: tuple[tuple[int, ...], ...]


def prepare_sharegpt(dataset: Path, policy_path: Path, tokenizer_path: str, output_dir: Path) -> dict[str, Any]:
    """Select by tokens and a declared seed; never read predictions or target measurements.

    Histories use corpus assistant messages, not fabricated model responses.
    Full messages are rendered together so special tokens follow the deployed
    chat template. The input corpus remains a local runtime asset.
    """

    # Transformers is needed for corpus preparation, not ordinary token replay.
    from transformers import AutoTokenizer

    policy = load_json(policy_path)
    records = load_json(dataset)
    tokenizer = AutoTokenizer.from_pretrained(tokenizer_path, trust_remote_code=True)
    random.Random(policy["seed"]).shuffle(records)
    selected_records = records[: policy["scan_conversations"]]
    reasons: Counter[str] = Counter()
    conversations = []
    seen_ids: set[str] = set()
    minimum, maximum = policy["minimum_input_tokens"], policy["maximum_input_tokens"]

    for record in selected_records:
        record_id = str(record["id"])
        # The cleaned split file names chunks <original-id>_<offset>. Keep
        # all chunks of one original conversation on the same side of holdout.
        prefix, separator, suffix = record_id.rpartition("_")
        identity = prefix if separator and suffix.isdigit() else record_id
        if identity in seen_ids:
            reasons["duplicate_conversation_id"] += 1
            continue
        seen_ids.add(identity)
        messages = []
        prompts = []
        for turn in record["conversations"]:
            role = {"human": "user", "gpt": "assistant"}.get(turn["from"])
            if role is None or not isinstance(turn["value"], str) or not turn["value"].strip():
                reasons["invalid_role_or_empty_message"] += 1
                break
            expected = "user" if len(messages) % 2 == 0 else "assistant"
            if role != expected:
                reasons["nonalternating_messages"] += 1
                break

            messages.append({"role": role, "content": turn["value"]})
            if role == "user":
                tokens = tokenizer.apply_chat_template(
                    messages, tokenize=True, return_dict=False, add_generation_prompt=True, enable_thinking=False
                )
                if not minimum <= len(tokens) <= maximum:
                    reasons["prompt_outside_declared_token_range"] += 1
                    break
                prompts.append(tuple(tokens))

        if prompts:
            conversations.append(Conversation(identity, record_id, tuple(prompts)))
        else:
            reasons["no_admitted_prompt"] += 1

    used: set[str] = set()
    independent = []
    boundaries = policy["independent_length_boundaries"]
    for lower, upper in zip(boundaries, boundaries[1:]):
        candidates = [c for c in conversations if lower <= len(c.prompts[0]) < upper and c.conversation_id not in used]
        chosen = _take(candidates, policy["independent_requests_per_bucket"], f"independent [{lower},{upper})")
        independent.extend((c, 0) for c in chosen)
        used.update(c.conversation_id for c in chosen)

    turns = policy["multi_turn_turns"]
    multi = _take(
        [c for c in conversations if len(c.prompts) >= turns and c.conversation_id not in used],
        policy["multi_turn_conversations"],
        "multi-turn conversations",
    )
    used.update(c.conversation_id for c in multi)
    revisit = _take(
        [
            c
            for c in conversations
            if len(c.prompts[-1]) >= policy["revisit_minimum_tokens"] and c.conversation_id not in used
        ],
        policy["revisit_conversations"],
        "revisited conversations",
    )
    used.update(c.conversation_id for c in revisit)

    workloads = {
        "independent": independent,
        "multi_turn": [(c, turn) for turn in range(turns) for c in multi],
        "revisit": [(c, len(c.prompts) - 1) for _ in range(policy["revisit_passes"]) for c in revisit],
    }
    # A disjoint real prompt warms the common eager/Decode path outside the
    # formal window. Its cache contents remain part of the declared setup.
    warmup = next(c for c in conversations if c.conversation_id not in used)
    used.add(warmup.conversation_id)

    # These untouched conversations are reserved before any score is inspected.
    held_out = [c for c in conversations if c.conversation_id not in used]
    confirmation = _take(held_out, policy["confirmation_conversations"], "confirmation reserve")
    used.update(c.conversation_id for c in confirmation)
    calibration = _take(
        [c for c in held_out if c.conversation_id not in used],
        policy["calibration_conversations"],
        "independent calibration reserve",
    )
    report = {
        "dataset": str(dataset),
        "selection_policy": policy,
        "corpus_conversation_count": len(records),
        "scanned_conversation_count": len(selected_records),
        "admitted_conversation_count": len(conversations),
        "filter_counts": dict(reasons),
        "filter_count_semantics": "Reasons count rejected messages/prompts or records; a valid earlier prefix can remain.",
        "truncated_text_count": 0,
        "history_semantics": "Corpus assistant messages are given historical context, not current model output.",
        "warmup": {
            "conversation_id": warmup.conversation_id,
            "record_id": warmup.record_id,
            "input_tokens": len(warmup.prompts[0]),
            "semantics": "One preparation request; retained cache; excluded from the formal HTTP window and training.",
        },
        "workloads": {},
        "confirmation_conversation_ids": [c.conversation_id for c in confirmation],
        "independent_calibration_conversation_ids": [c.conversation_id for c in calibration],
    }
    for name, requests in workloads.items():
        workload_id = f"sharegpt_{name}"
        template_path = output_dir / f"{workload_id}.json"
        document = _request_template(workload_id, requests, policy["output_tokens"][name], warmup)
        write_json(template_path, document)
        lengths = [len(c.prompts[turn]) for c, turn in requests]
        prefix_lengths = []
        previous: dict[str, tuple[int, ...]] = {}
        for conversation, turn in requests:
            tokens = conversation.prompts[turn]
            if conversation.conversation_id in previous:
                prefix_lengths.append(_common_prefix(previous[conversation.conversation_id], tokens))
            previous[conversation.conversation_id] = tokens
        report["workloads"][workload_id] = {
            "template": str(template_path),
            "formal_request_count": len(requests),
            "output_tokens_per_request": policy["output_tokens"][name],
            "input_tokens": {"minimum": min(lengths), "median": statistics.median(lengths), "maximum": max(lengths)},
            "shared_input_prefix_tokens": prefix_lengths,
            "requests": [
                {"conversation_id": c.conversation_id, "record_id": c.record_id, "user_turn": turn}
                for c, turn in requests
            ],
        }

    write_json(output_dir / "selection_report.json", report)
    return report


def _take(values: list[Conversation], count: int, description: str) -> list[Conversation]:
    if len(values) < count:
        raise TemplateValidationError(
            f"ShareGPT selection needs {count} {description}, only {len(values)} are available"
        )

    return values[:count]


def _common_prefix(left: tuple[int, ...], right: tuple[int, ...]) -> int:
    for index, (a, b) in enumerate(zip(left, right)):
        if a != b:
            return index

    return min(len(left), len(right))


def _request_template(
    workload_id: str, requests: list[tuple[Conversation, int]], output_tokens: int, warmup: Conversation
) -> dict[str, Any]:
    definitions = {"warmup": {"prompt_token_ids": list(warmup.prompts[0])}}
    steps = [{"id": "warmup", "kind": "request", "request": "warmup", "phase": "prepare", "measure": False}]
    for index, (conversation, turn) in enumerate(requests):
        name = f"request_{index:03d}"
        definitions[name] = {"prompt_token_ids": list(conversation.prompts[turn])}
        steps.append({"id": name, "kind": "request", "request": name, "phase": "formal", "measure": True})

    return {
        "id": workload_id,
        "description": "Serial real-text requests with given conversation history and controlled token replay.",
        "defaults": {
            "sampling": {
                "max_new_tokens": output_tokens,
                "temperature": 0,
                "top_p": 1.0,
                "top_k": 1,
                "ignore_eos": True,
            }
        },
        "fragments": {},
        "request_defs": definitions,
        "steps": steps,
        "formal_window": {"start_step": steps[1]["id"], "end_step": steps[-1]["id"]},
    }
