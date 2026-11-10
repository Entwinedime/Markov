"""Build three reproducible full-contract access patterns from original CUAD questions."""

from __future__ import annotations

import argparse
from collections import Counter
import json
from pathlib import Path
import random
import statistics

from markov.artifacts.writing import write_json


INSTRUCTION = "Review the following contract and answer the question briefly using only the contract."
PREPARATION_LENGTHS = (256, 512, 768, 1024, 1536, 2048, 512, 1024)


def request(tokens: list[int], output: int) -> dict:
    return {"input_ids": tokens, "max_new_tokens": output, "ignore_eos": True, "return_logprob": False}


def prefix_statistics(requests: list[dict]) -> dict:
    trie, nodes, reuse = {}, 0, []
    for row in requests:
        branch, matched, existing = trie, 0, True
        for token in row["input_ids"]:
            if token not in branch:
                branch[token] = {}
                nodes += 1
                existing = False
            if existing:
                matched += 1
            branch = branch[token]
        reuse.append(matched)
    return {
        "distinct_prefix_tree_tokens": nodes,
        "unbounded_cache_reusable_tokens": sum(reuse),
        "unbounded_cache_reusable_paged_tokens": {
            str(page): sum(n // page * page for n in reuse) for page in (32, 64, 128)
        },
    }


def prepare(dataset: Path, model: Path, output: Path) -> dict:
    from transformers import AutoTokenizer

    tokenizer = AutoTokenizer.from_pretrained(model, local_files_only=True)
    source = json.loads(dataset.read_text(encoding="utf-8"))
    qualified, census = [], []
    for contract in source["data"]:
        paragraphs = contract["paragraphs"]
        if len(paragraphs) != 1:
            raise ValueError("This example requires CUAD's original one-full-contract record")

        context = paragraphs[0]["context"]
        body_tokens = len(tokenizer.encode(context, add_special_tokens=False))
        census.append({"title": contract["title"], "body_tokens": body_tokens})
        if not 4096 <= body_tokens <= 7000:
            continue

        questions, seen = [], set()
        for qa in paragraphs[0]["qas"]:
            question = qa["question"]
            if question in seen:
                continue
            seen.add(question)
            content = f"{INSTRUCTION}\n\nContract:\n{context}\n\nQuestion:\n{question}"
            tokens = tokenizer.apply_chat_template(
                [{"role": "user", "content": content}],
                tokenize=True,
                add_generation_prompt=True,
                enable_thinking=False,
                return_dict=False,
            )
            if len(tokens) > 7168:
                continue

            answers = qa["answers"]
            questions.append(
                {
                    "id": qa["id"],
                    "question": question,
                    "input_ids": tokens,
                    "answer_tokens": [len(tokenizer.encode(a["text"], add_special_tokens=False)) for a in answers],
                    "has_answer": bool(answers),
                }
            )
        if len(questions) >= 32:
            qualified.append({"title": contract["title"], "body_tokens": body_tokens, "questions": questions})

    output.mkdir(parents=True, exist_ok=True)
    write_json(output / "census.json", {"contracts": census, "qualified_contracts": len(qualified)})
    if len(qualified) < 32:
        raise ValueError(f"Need 32 eligible full contracts with 32 distinct original questions; found {len(qualified)}")

    qualified.sort(key=lambda row: row["title"])
    random.Random(20261110).shuffle(qualified)
    contracts = qualified[:32]
    round_robin = [(document, question) for question in range(16) for document in range(32)]
    scenes = {
        "alternating": [(document, question) for question in range(32) for document in range(16)],
        "long_revisit": round_robin,
        "group_switch": [
            (document, question)
            for first_question in range(0, 16, 4)
            for group in range(0, 32, 4)
            for question in range(first_question, first_question + 4)
            for document in range(group, group + 4)
        ],
    }
    if Counter(scenes["long_revisit"]) != Counter(scenes["group_switch"]):
        raise ValueError("The order comparison must preserve exactly the same request set")

    preparation = [request([100 + index] * length, 8) for index, length in enumerate(PREPARATION_LENGTHS)]
    reports = {}
    for name, order in scenes.items():
        if len(order) != 512 or len(set(order)) != 512:
            raise ValueError("Each scene must have 512 different contract/question pairs")

        formal, identities, last, distances = [], [], {}, []
        for position, (document, question) in enumerate(order):
            contract = contracts[document]
            qa = contract["questions"][question]
            formal.append(request(qa["input_ids"], 32))
            identities.append(
                {
                    "title": contract["title"],
                    "id": qa["id"],
                    "question": qa["question"],
                    "body_tokens": contract["body_tokens"],
                    "input_tokens": len(qa["input_ids"]),
                    "has_answer": qa["has_answer"],
                    "answer_tokens": qa["answer_tokens"],
                }
            )
            if document in last:
                distances.append(position - last[document])
            last[document] = position

        directory = output / name
        directory.mkdir(exist_ok=True)
        write_json(
            directory / "workload.json",
            {
                "requests": preparation + formal,
                "formal_begin_index": len(preparation),
                "drain_before_formal": True,
            },
        )
        write_json(directory / "identities.json", {"requests": identities})
        lengths = [row["input_tokens"] for row in identities]
        answers = [length for row in identities for length in row["answer_tokens"]]
        reports[name] = {
            "requests": len(formal),
            "contracts": len(last),
            "input_tokens_min": min(lengths),
            "input_tokens_max": max(lengths),
            "input_tokens_total": sum(lengths),
            "body_tokens_working_set": sum(contracts[d]["body_tokens"] for d in last),
            "first_accesses": len(last),
            "revisit_distance_min": min(distances),
            "revisit_distance_median": statistics.median(distances),
            "revisit_distance_max": max(distances),
            "questions_with_answer": sum(row["has_answer"] for row in identities),
            "reference_answers": len(answers),
            "reference_answers_over_32_tokens": sum(n > 32 for n in answers),
            **prefix_statistics(formal),
        }

    summary = {
        "dataset": str(dataset.resolve()),
        "dataset_source": "https://github.com/The-Atticus-Project/cuad",
        "model": str(model),
        "seed": 20261110,
        "selection": "Full original context; 4096–7000 body tokens; complete prompt <=7168",
        "instruction": INSTRUCTION,
        "formal_output_tokens": 32,
        "scenes": reports,
        "limitations": "Constructed serial order, fixed output; working-set and revisit statistics are reuse potential, not measured hit rate.",
    }
    write_json(output / "summary.json", summary)
    return summary


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset", type=Path, required=True)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(prepare(args.dataset, args.model, args.output), ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
