"""Convert consecutive rows of the Azure 2024 inference trace to a Markov workload."""

import argparse
import csv
from datetime import datetime, timedelta
from itertools import islice
from pathlib import Path

from markov_adapters.artifacts import write_json
from markov_adapters.sglang.configuration import ServingInput


def convert(path: Path, first_row: int, count: int) -> dict:
    requests = []
    origin = previous = None
    with path.open(encoding="utf-8", newline="") as stream:
        rows = csv.DictReader(stream)
        if rows.fieldnames != ["TIMESTAMP", "ContextTokens", "GeneratedTokens"]:
            raise ValueError("expected the Azure 2024 inference trace columns")

        for index, row in enumerate(islice(rows, first_row - 1, first_row - 1 + count), first_row):
            arrival = datetime.fromisoformat(row["TIMESTAMP"])
            if arrival.utcoffset() is None or (previous is not None and arrival < previous):
                raise ValueError(f"Azure row {index} has an unordered or timezone-free timestamp")

            # Integer arithmetic preserves the trace's microsecond precision and tied arrivals.
            if origin is None:
                origin = arrival
            requests.append({
                "id": f"azure-{index}",
                "arrival_ns": ((arrival - origin) // timedelta(microseconds=1)) * 1000,
                "prompt_tokens": int(row["ContextTokens"]),
                "output_tokens": int(row["GeneratedTokens"]),
            })
            previous = arrival

    if len(requests) != count:
        raise ValueError(f"the trace has fewer than {count} rows starting at {first_row}")

    return ServingInput.workload_input({"id": f"azure-{first_row}-{count}", "requests": requests})


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--first-row", type=int, required=True, help="one-based data row, excluding the header")
    parser.add_argument("--requests", type=int, required=True)
    args = parser.parse_args()
    if args.first_row <= 0 or args.requests <= 0:
        parser.error("the first data row and request count must be positive")

    write_json(args.output, convert(args.trace, args.first_row, args.requests), replace=False)


if __name__ == "__main__":
    main()
