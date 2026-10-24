#!/usr/bin/env python3
"""A bounded discovery recipe using the formal native session."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from markov_collection.adapters.frameworks.sglang.acquisition import SGLangAcquisition
from markov_collection.adapters.frameworks.sglang.session import NativeServingSession

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--profile", action="store_true")
    parser.add_argument("--dtype", default="bfloat16", help="native model dtype; must match the prediction assets")
    parser.add_argument("--tp-size", type=int, default=1)
    parser.add_argument("--rank", type=int, default=0)
    parser.add_argument("--input", type=Path, help="serving input with simultaneous requests and native pool limits")
    parser.add_argument(
        "--matrix-selector", required=True, help="native matrix metadata executable from the observer build"
    )

    arguments = parser.parse_args()
    if arguments.tp_size < 1 or not 0 <= arguments.rank < arguments.tp_size:
        parser.error("rank must be in [0, tp-size)")

    requests = NativeServingSession.warmup_requests(128)
    configuration = SGLangAcquisition.configuration(128)
    if arguments.input is not None:
        specification = json.loads(arguments.input.read_text())
        if any(request["arrival_ns"] != 0 for request in specification["requests"]):
            parser.error("native observation submits requests together; arrival_ns must be zero")
        requests = tuple((request["prompt_tokens"], request["output_tokens"]) for request in specification["requests"])
        configuration = specification["configuration"]

    NativeServingSession(
        arguments.model,
        requests,
        arguments.output,
        arguments.profile,
        arguments.tp_size,
        arguments.rank,
        configuration=configuration,
        matrix_selection={
            "binary": arguments.matrix_selector,
            "limits": SGLangAcquisition.row_limits([configuration], max(prompt for prompt, _ in requests)),
        },
        native_library=str(Path(arguments.matrix_selector).resolve().with_name(SGLangAcquisition.observer_library)),
        dtype=arguments.dtype,
    ).run()
    SGLangAcquisition.interpret({"output": str(arguments.output), "rank": arguments.rank})
