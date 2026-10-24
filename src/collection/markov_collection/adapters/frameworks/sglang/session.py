"""Native SGLang session; the framework owns requests, KV and worker lifetimes."""

from __future__ import annotations

import json
import os
import time
from pathlib import Path

from markov_collection.artifacts.json_file import write_json


class NativeServingSession:
    def __init__(
        self,
        model: str,
        requests: tuple[tuple[int, int], ...],
        output: Path,
        profile: bool,
        tp_size: int,
        rank: int,
        *,
        configuration: dict[str, int],
        matrix_selection: dict[str, object],
        native_library: str,
        dtype: str = "bfloat16",
        attention_backend: str = "ascend",
    ) -> None:
        self.model = model
        self.requests = requests
        self.output = output.resolve()
        self.profile = profile
        self.tp_size = tp_size
        self.rank = rank
        self.dtype = dtype
        self.attention_backend = attention_backend
        self.configuration = configuration
        self.matrix_selection = matrix_selection
        self.native_library = native_library

    @staticmethod
    def warmup_requests(page_size: int) -> tuple[tuple[int, int], ...]:

        # Keep warmup independent of target requests while covering page and batch transitions.
        return (
            (max(1, page_size - 1), 4),
            (page_size + 1, 1),
            (page_size // 2 + 1, 3),
            (page_size // 4 + 1, 2),
            (1, 1),
        )

    def generate(self, engine: object, prefix: str, requests: tuple[tuple[int, int], ...]) -> None:
        response = engine.generate(
            input_ids=[[1] * prompt for prompt, _ in requests],
            sampling_params=[
                {"max_new_tokens": output, "temperature": 0, "ignore_eos": True} for _, output in requests
            ],
            rid=[f"{prefix}{index}" for index in range(len(requests))],
            stream=True,
        )

        # Consume native streaming to completion; publication then contains actual token-visible ports.
        for _ in response:
            pass

    @staticmethod
    def reset(engine: object) -> None:
        response = engine.loop.run_until_complete(engine.tokenizer_manager.flush_cache(timeout_s=30))
        if not response.success:
            raise RuntimeError(f"native idle/cache reset failed: {response.message}")

    def run(self) -> None:
        warmup = self.warmup_requests(self.configuration["page_size"])
        self.output.mkdir(parents=True, exist_ok=True)
        (self.output / "runtime").mkdir(exist_ok=True)
        description = self.output / "session.json"
        session = {
            "output": str(self.output),
            "source_requests": [f"capture/{index}" for index in range(len(self.requests))],
            "baseline_requests": [f"baseline/{index}" for index in range(len(self.requests))],
            "matrix_selection": self.matrix_selection,
            "native_library": self.native_library,
        }
        if self.tp_size > 1:
            session["shard"] = {"size": self.tp_size, "rank": self.rank}
        if description.exists():
            raise FileExistsError(f"native session already exists: {description}")
        write_json(description, session)
        os.environ["MARKOV_GRAPH_SESSION"] = str(description)
        os.environ["SGLANG_PLUGINS"] = "markov"

        from sglang.srt.entrypoints.engine import Engine

        started = time.perf_counter_ns()
        engine = Engine(
            model_path=self.model,
            skip_tokenizer_init=True,
            device="npu",
            dtype=self.dtype,
            tp_size=1,
            load_format="dummy",
            attention_backend=self.attention_backend,
            **self.configuration,
            chunked_prefill_size=-1,
            disable_radix_cache=True,
            disable_overlap_schedule=True,
            disable_cuda_graph=True,
            mem_fraction_static=0.8,
            log_level="warning",
        )
        initialized = time.perf_counter_ns()
        try:
            self.generate(engine, "warmup/", warmup)
            self.reset(engine)
            warmed = time.perf_counter_ns()
            if self.profile:
                engine.start_profile(
                    output_dir=str(self.output / "native"),
                    activities=["CPU", "GPU"],
                    record_shapes=False,
                    with_stack=False,
                )
            try:
                self.generate(engine, "baseline/", self.requests)
                self.reset(engine)
                self.generate(engine, "capture/", self.requests)
                self.reset(engine)
            finally:
                if self.profile:
                    engine.stop_profile()
            captured = time.perf_counter_ns()
        finally:
            engine.shutdown()

        summary = {
            "initialization_ns": initialized - started,
            "warmup_ns": warmed - initialized,
            "warmup_requests": warmup,
            "capture_and_export_ns": captured - warmed,
            "profile": self.profile,
            "model": self.model,
            "weight_source": "native_dummy_loader_full_model",
            "logical_tp": self.tp_size,
            "rank": self.rank,
            "configuration": self.configuration,
            "matrix_selection": self.matrix_selection,
            "worker_limits": engine._scheduler_init_result.scheduler_infos,
        }
        write_json(self.output / "session-summary.json", summary, indent=2)
        print(json.dumps(summary))
