"""Shared fixed warmup and native worker failure handling."""

import signal
from typing import Any

from markov_collection.instrumentation.session import CaptureSession


class SGLangSession(CaptureSession):
    def _warmup(self, engine: Any, request_ids: set[str]) -> dict[str, Any]:
        identity = "markov-warmup"
        while identity in request_ids:
            identity += "_"

        inputs = [1]
        parameters = {"max_new_tokens": 2, "ignore_eos": True, "temperature": 0}
        # One prefill and one decode precede every observation window.
        engine.generate(input_ids=inputs, sampling_params=parameters, rid=identity)

        # Native tokenizer setup installs an asyncio SIGQUIT handler on first use.
        engine.loop.remove_signal_handler(signal.SIGQUIT)
        signal.signal(signal.SIGQUIT, self._handle_worker_failure)

        self._reset(engine)
        return {"input_tokens": len(inputs), "input_ids": inputs, "sampling_parameters": parameters}

    def _reset(self, engine: Any) -> None:
        # Client replies can precede native batch cleanup. The native deferred
        # RPC waits for scheduler idleness; the attempt monitor owns the deadline.
        result = engine.loop.run_until_complete(
            engine.tokenizer_manager.flush_cache(timeout_s=self.prepared["permission"]["max_wall_time_sec"])
        )
        if not result.success:
            raise RuntimeError(f"native request/cache reset failed: {result.message}")
