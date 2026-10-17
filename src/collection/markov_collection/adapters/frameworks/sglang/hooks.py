"""Select the requested observation purpose at SGLang's plugin entry point."""

import json
import os
from pathlib import Path


def install() -> None:
    prepared_input = os.environ.get("MARKOV_PREPARED_INPUT")
    if prepared_input is None:
        return

    with Path(prepared_input).open(encoding="utf-8") as stream:
        prepared = json.load(stream)

    # The worker imports one observation path. Performance capture does not
    # load model-region bindings or install semantic instrumentation.
    if prepared["role"] == "validation" and prepared["observation_purpose"] == "ports":
        from markov_collection.adapters.frameworks.sglang.port_observation import install
    elif prepared["role"] != "performance":
        from markov_collection.adapters.frameworks.sglang.observer import install
    elif prepared["measurement_kind"] == "transfer":
        from markov_collection.adapters.frameworks.sglang.transfer_sampling import install
    else:
        from markov_collection.adapters.frameworks.sglang.device_sampling import install

    install(prepared)
