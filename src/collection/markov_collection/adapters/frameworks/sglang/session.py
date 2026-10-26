"""Native SGLang session; the framework owns requests, KV and worker lifetimes."""

from __future__ import annotations

import json
import os
import time
from collections.abc import Iterable
from pathlib import Path
from typing import Any

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
        pp_size: int = 1,
        ep_size: int = 1,
        dtype: str = "bfloat16",
        attention_backend: str = "ascend",
        observe_all_workers: bool = False,
        serving_input: dict[str, Any] | None = None,
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
        if (
            serving_input is not None
            and tuple((request["prompt_tokens"], request["output_tokens"]) for request in serving_input["requests"])
            != requests
        ):
            raise ValueError("injected workload must describe the submitted native requests")

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

    @staticmethod
    def check_responses(responses: Iterable[dict[str, Any]], expected: dict[str, int]) -> None:
        completed = set()
        for response in responses:
            metadata = response["meta_info"]
            reason = metadata["finish_reason"]
            if reason is None:
                continue

            identity = metadata["id"]
            if reason["type"] != "length" or metadata["completion_tokens"] != expected[identity]:
                raise RuntimeError(f"native request {identity} did not finish its fixed output: {reason}")
            completed.add(identity)

        if completed != expected.keys():
            raise RuntimeError(f"native stream ended before requests completed: {sorted(expected.keys() - completed)}")

    def generate(self, engine: object, prefix: str, requests: tuple[tuple[int, int], ...]) -> None:
        print(f"markov: native {prefix}begin", flush=True)
        response = engine.generate(
            input_ids=[[1] * prompt for prompt, _ in requests],
            sampling_params=[
                {"max_new_tokens": output, "temperature": 0, "ignore_eos": True} for _, output in requests
            ],
            rid=[f"{prefix}{index}" for index in range(len(requests))],
            stream=True,
        )

        # HTTP success and a closed stream can still contain aborted native requests.
        self.check_responses(response, {f"{prefix}{index}": output for index, (_, output) in enumerate(requests)})
        print(f"markov: native {prefix}complete", flush=True)

    @staticmethod
    def reset(engine: object) -> None:
        print("markov: native pool reset begin", flush=True)
        response = engine.loop.run_until_complete(engine.tokenizer_manager.flush_cache(timeout_s=30))
        if not response.success:
            raise RuntimeError(f"native idle/cache reset failed: {response.message}")
        print("markov: native pool reset complete", flush=True)

    def wait_for_sources(self) -> None:
        sources = [self.output / f"rank-{rank}.json" for rank in self.observed_roles]

        # PP forwards control requests after replying locally; every role must publish before shutdown.
        while not all(path.is_file() for path in sources):
            time.sleep(0.05)
        print("markov: all native role sources complete", flush=True)

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
            "rank_offset": 0,
            "pp_size": self.pp_size,
            "ep_size": self.ep_size,
            "observe_all_workers": self.observe_all_workers,
            "serving_input": self.serving_input,
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
            **self.configuration,
            chunked_prefill_size=-1,
            disable_radix_cache=True,
            disable_overlap_schedule=True,
            disable_cuda_graph=True,
            mem_fraction_static=0.8,
            log_level="warning",
        )
        initialized = time.perf_counter_ns()
        if self.native_group:
            plan = json.loads((self.output / "role-plan.json").read_text())
            self.observed_roles = tuple(plan["observed_roles"])
        print("markov: native initialization complete", flush=True)
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
                self.wait_for_sources()
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
            "routing_source": "workload" if self.serving_input is not None else "native",
        }
        write_json(self.output / "session-summary.json", summary, indent=2)
        print(json.dumps(summary))
