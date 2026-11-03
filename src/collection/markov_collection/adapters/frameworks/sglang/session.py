"""Native SGLang session; the framework owns requests, KV and worker lifetimes."""

from __future__ import annotations

import json
import os
import time
from collections.abc import Sequence
from pathlib import Path
from typing import Any

from markov_collection.adapters.frameworks.sglang.submissions import EngineSubmissions
from markov_collection.adapters.frameworks.sglang.window import ObservationWindow
from markov_collection.artifacts import write_json


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
        pp_size: int = 1,
        ep_size: int = 1,
        dtype: str = "bfloat16",
        attention_backend: str = "ascend",
        observe_all_workers: bool = False,
        serving_input: dict[str, Any] | None = None,
        submissions: Sequence[Sequence[int]] | None = None,
    ) -> None:

        self.native_group = pp_size > 1 or ep_size > 1 or observe_all_workers
        self.roles = self.source_roles(tp_size, pp_size, rank, ep_size)
        if self.native_group:
            self.roles = range(tp_size * pp_size)
        self.observed_roles = tuple(self.roles)
        self.model = model
        self.requests = requests
        self.output = output.resolve()
        self.profile = profile
        self.tp_size = tp_size
        self.rank = rank
        self.pp_size = pp_size
        self.ep_size = ep_size
        self.dtype = dtype
        self.attention_backend = attention_backend
        self.configuration = configuration
        self.matrix_selection = matrix_selection
        self.native_library = native_library
        self.observe_all_workers = observe_all_workers
        self.serving_input = serving_input
        self.submissions = [list(range(len(requests)))] if submissions is None else submissions
        if (
            serving_input is not None
            and tuple((request["prompt_tokens"], request["output_tokens"]) for request in serving_input["requests"])
            != requests
        ):
            raise ValueError("injected workload must describe the submitted native requests")
        self.inputs = EngineSubmissions(self.content_input(requests, serving_input), self.submissions)
        self.deliveries: dict[str, Any] = {}

    @staticmethod
    def source_roles(tp: int, pp: int, rank: int, ep: int = 1) -> range:

        # The pinned upstream Ascend capability table explicitly excludes PP2.
        if pp == 2:
            raise ValueError("this SGLang Ascend implementation does not support PP2")
        if ep > 1 and ep != tp:
            raise ValueError("this SGLang DeepEP implementation requires EP to equal TP")
        return range(tp * pp) if pp > 1 or ep > 1 else range(rank, rank + 1)

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

    @classmethod
    def structure_requests(cls, page_size: int) -> tuple[tuple[int, int], ...]:
        *initial, tail = cls.warmup_requests(page_size)

        # Page-aligned input and a live tail cover identity padding and native batch merge.
        initial[2] = (page_size, initial[2][1])
        return (*initial, (tail[0], 3))

    def generate(self, engine: object, prefix: str, requests: tuple[tuple[int, int], ...]) -> None:
        print(f"markov: native {prefix} begin", flush=True)
        inputs = (
            EngineSubmissions(self.content_input(requests), [list(range(len(requests)))])
            if prefix == "warmup"
            else self.inputs
        )
        inputs.content.check_vocabulary(engine.tokenizer_manager.model_config.vocab_size)
        try:
            engine.loop.run_until_complete(inputs.run(engine, prefix))
        finally:

            # Actual submission delay is evidence; it never rewrites the declared workload.
            self.deliveries[prefix] = inputs.delivery
            write_json(self.output / "request-delivery.json", self.deliveries, indent=2)
        print(f"markov: native {prefix} complete", flush=True)

    @staticmethod
    def content_input(requests, source=None) -> dict:
        return {
            "requests": source["requests"]
            if source is not None
            else [
                {"id": str(index), "arrival_ns": 0, "prompt_tokens": prompt, "output_tokens": output}
                for index, (prompt, output) in enumerate(requests)
            ],
            "prefixes": source.get("prefixes", []) if source is not None else [],
        }

    def reset(self, engine: object, phase: str) -> None:
        print("markov: native pool reset begin", flush=True)
        ObservationWindow.request_drain(self.output, phase)
        response = engine.loop.run_until_complete(engine.tokenizer_manager.flush_cache(timeout_s=30))
        if not response.success:
            raise RuntimeError(f"native idle/cache reset failed: {response.message}")
        ObservationWindow.wait(self.output, self.roles, phase, "complete")
        print("markov: native pool reset complete", flush=True)

    def run(self) -> None:
        warmup = self.warmup_requests(self.configuration["page_size"])
        self.output.mkdir(parents=True, exist_ok=True)
        (self.output / "runtime").mkdir(exist_ok=True)
        description = self.output / "session.json"
        session = {
            "output": str(self.output),
            "warmup_requests": [f"warmup/{index}" for index in range(len(warmup))],
            "source_requests": [f"capture/{index}" for index in range(len(self.requests))],
            "baseline_requests": [f"baseline/{index}" for index in range(len(self.requests))],
            "matrix_selection": self.matrix_selection,
            "native_library": self.native_library,
            "rank_offset": 0,
            "native_participants": list(self.roles),
            "pp_size": self.pp_size,
            "ep_size": self.ep_size,
            "observe_all_workers": self.observe_all_workers,
            "serving_input": self.serving_input,
            "content": self.content_input(self.requests, self.serving_input),
            "submissions": self.submissions,
        }
        if self.tp_size > 1 and not self.native_group:
            session["shard"] = {"size": self.tp_size, "rank": self.rank}
        if description.exists():
            raise FileExistsError(f"native session already exists: {description}")
        write_json(description, session)
        os.environ["MARKOV_GRAPH_SESSION"] = str(description)
        os.environ["SGLANG_PLUGINS"] = "markov"

        expert_options = {}

        # Select the qualified native all-to-all implementation before workers start.
        if self.ep_size > 1:
            os.environ.update(DEEP_USE_ALLTOALL_MODE="1", DEEPEP_HCCL_BUFFSIZE="1024", HCCL_BUFFSIZE="1024")
            expert_options = {"ep_size": self.ep_size, "moe_a2a_backend": "deepep", "deepep_mode": "normal"}

        from sglang.srt.entrypoints.engine import Engine

        started = time.perf_counter_ns()
        engine = Engine(
            model_path=self.model,
            skip_tokenizer_init=True,
            device="npu",
            dtype=self.dtype,
            tp_size=self.tp_size if self.native_group else 1,
            pp_size=self.pp_size,
            **expert_options,
            load_format="dummy",
            attention_backend=self.attention_backend,
            **{"chunked_prefill_size": -1, "disable_radix_cache": True, **self.configuration},
            disable_overlap_schedule=True,
            disable_cuda_graph=True,
            sleep_on_idle=True,
            mem_fraction_static=0.8,
            log_level="warning",
        )
        try:
            initialized = time.perf_counter_ns()
            if self.native_group:
                plan = json.loads((self.output / "role-plan.json").read_text())
                self.observed_roles = tuple(plan["observed_roles"])
            print("markov: native initialization complete", flush=True)

            self.generate(engine, "warmup", warmup)
            self.reset(engine, "warmup")
            warmed = time.perf_counter_ns()
            if self.profile:
                engine.start_profile(
                    output_dir=str(self.output / "native"),
                    activities=["CPU", "GPU"],
                    record_shapes=False,
                    with_stack=False,
                )
                ObservationWindow.wait(self.output, self.roles, "profiler", "complete")
            try:
                self.generate(engine, "baseline", self.requests)
                self.reset(engine, "baseline")
                self.generate(engine, "capture", self.requests)
                self.reset(engine, "source")
            finally:
                if self.profile:
                    engine.stop_profile()
                    if self.native_group:
                        directories = [self.output / "native" / f"rank-{rank}" for rank in self.observed_roles]
                        while not all(directory.is_dir() for directory in directories):
                            time.sleep(0.05)
            captured = time.perf_counter_ns()
        finally:
            engine.shutdown()

        summary = {
            "initialization_ns": initialized - started,
            "warmup_ns": warmed - initialized,
            "warmup_requests": warmup,
            "capture_and_flush_ns": captured - warmed,
            "profile": self.profile,
            "model": self.model,
            "weight_source": "native_dummy_loader_full_model",
            "logical_tp": self.tp_size,
            "logical_pp": self.pp_size,
            "logical_ep": self.ep_size,
            "rank": self.rank,
            "native_participants": list(self.roles),
            "observed_roles": list(self.observed_roles),
            "configuration": self.configuration,
            "matrix_selection": self.matrix_selection,
            "worker_limits": engine._scheduler_init_result.scheduler_infos,
            "routing_source": "workload" if (self.serving_input or {}).get("ep_routes") else "native",
            "content_source": "declared_prefixes_and_private_continuations",
        }
        write_json(self.output / "session-summary.json", summary, indent=2)
        print(json.dumps(summary))
