"""Select the native capture adapter at the workflow boundary."""


def capture_adapter(framework: str):
    if framework == "sglang":
        from markov.adapters.sglang import capture
    elif framework == "ktransformers":
        from markov.adapters.ktransformers import capture
    else:
        raise ValueError(f"Inference capture does not support framework {framework}")

    return capture
