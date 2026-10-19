"""Select the requested observation purpose at SGLang's plugin entry point."""

import os
from pathlib import Path

from markov_collection.artifacts.json_file import read_json


def install() -> None:
    prepared_input = os.environ.get("MARKOV_PREPARED_INPUT")
    if prepared_input is None:
        return

    prepared = read_json(Path(prepared_input))

    from sglang.srt.plugins.hook_registry import HookRegistry, HookType

    from markov_collection.adapters.frameworks.sglang.warmup import SGLangWarmup
    from markov_collection.adapters.runtimes.ascend.compilation import CompilationObservation

    directory = Path(os.environ["MARKOV_OBSERVATIONS"]).parent
    compilation = CompilationObservation(directory / "compilation")
    HookRegistry.register("triton.runtime.jit.JITFunction._do_compile", compilation.compile, HookType.AROUND)
    HookRegistry.register(
        "triton.compiler.compiler.CompiledKernel._init_handles", compilation.initialize, HookType.AROUND
    )
    warmup = SGLangWarmup(directory / "warmup", prepared["warmup_prefix"])
    HookRegistry.register("sglang.srt.managers.scheduler.Scheduler.run_batch", warmup.execute, HookType.AROUND)
    HookRegistry.register("sglang.srt.managers.scheduler.Scheduler.flush_cache", warmup.reset, HookType.AROUND)

    # The worker imports one observation path. Performance capture does not
    # load model-region bindings or install semantic instrumentation.
    if prepared["role"] == "validation" and prepared["observation_purpose"] == "ports":
        from markov_collection.adapters.frameworks.sglang.port_observation import install
    elif prepared["role"] != "performance":
        from markov_collection.adapters.frameworks.sglang.observer import install
    elif prepared["measurement_kind"] in {"transfer", "cpu"}:
        from markov_collection.adapters.frameworks.sglang.host_sampling import install
    else:
        from markov_collection.adapters.frameworks.sglang.device_sampling import install

    install(prepared)
