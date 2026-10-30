#!/usr/bin/env python3
"""A bounded discovery recipe using the formal native session."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

from markov_collection.adapters.frameworks.sglang.acquisition import SGLangAcquisition
from markov_collection.adapters.frameworks.sglang.session import NativeServingSession


def native_configuration(settings: dict[str, Any]) -> dict[str, int]:
    configuration = {name: settings[name] for name in ("page_size", "max_running_requests", "max_total_tokens")}
    if "max_prefill_tokens" in settings:
        configuration["max_prefill_tokens"] = settings["max_prefill_tokens"]
    if "prefill_chunk_tokens" in settings:
        configuration["chunked_prefill_size"] = settings["prefill_chunk_tokens"]
    if "mixed_batches" in settings:
        configuration["enable_mixed_chunk"] = settings["mixed_batches"]
    if "prefix_cache" in settings:
        configuration["disable_radix_cache"] = not settings["prefix_cache"]
    if settings.get("pp", 1) > 1:
        configuration["pp_async_batch_depth"] = 0
        if "microbatch_requests" in settings:
            configuration["pp_max_micro_batch_size"] = settings["microbatch_requests"]
    return configuration


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--profile", action="store_true")
    parser.add_argument("--dtype", default="bfloat16", help="native model dtype; must match the prediction assets")
    parser.add_argument("--tp-size", type=int, default=1)
    parser.add_argument(
        "--pp-size", type=int, default=1, help="observe actual pipeline ranks together on visible devices"
    )
    parser.add_argument("--rank", type=int, default=0)
    parser.add_argument(
        "--observe-all-workers", action="store_true", help="observe native peers without changing role reuse"
    )
    parser.add_argument("--input", type=Path, help="serving input with simultaneous requests and native pool limits")
    parser.add_argument(
        "--matrix-selector", required=True, help="native matrix metadata executable from the observer build"
    )

    arguments = parser.parse_args()
    requests = NativeServingSession.structure_requests(128)
    tp_size, pp_size, ep_size = arguments.tp_size, arguments.pp_size, 1
    configuration = SGLangAcquisition.configuration(128, pp_size)
    range_configuration = configuration
    serving_input = None
    if arguments.input is not None:
        specification = json.loads(arguments.input.read_text())
        if any(request["arrival_ns"] != 0 for request in specification["requests"]):
            parser.error("native observation submits requests together; arrival_ns must be zero")
        requests = tuple((request["prompt_tokens"], request["output_tokens"]) for request in specification["requests"])
        settings = specification["configuration"]
        range_configuration = settings
        pp_size = settings.get("pp", pp_size)
        ep_size = settings.get("ep", {}).get("size", 1)
        configuration = native_configuration({**settings, "pp": pp_size})
        serving_input = specification

    if tp_size < 1 or not 0 <= arguments.rank < tp_size:
        parser.error("rank must be in [0, tp-size)")

    NativeServingSession(
        arguments.model,
        requests,
        arguments.output,
        arguments.profile,
        tp_size,
        arguments.rank,
        configuration=configuration,
        matrix_selection={
            "binary": arguments.matrix_selector,
            "limits": SGLangAcquisition.row_limits([range_configuration], max(prompt for prompt, _ in requests)),
        },
        native_library=str(Path(arguments.matrix_selector).resolve().with_name(SGLangAcquisition.observer_library)),
        pp_size=pp_size,
        ep_size=ep_size,
        dtype=arguments.dtype,
        observe_all_workers=arguments.observe_all_workers,
        serving_input=serving_input,
    ).run()
    SGLangAcquisition.interpret({"output": str(arguments.output), "rank": arguments.rank})
