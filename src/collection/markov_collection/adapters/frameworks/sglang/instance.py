"""Own one native Engine and its observation lifecycle within a capture job."""

from __future__ import annotations

import json
import os
import time
from pathlib import Path
from typing import Any

from markov_adapters.sglang.configuration import CaptureInput, InstanceConfiguration
from markov_collection.adapters.frameworks.sglang.profiles import NativeProfiles
from markov_collection.adapters.frameworks.sglang.submissions import EngineSubmissions
from markov_collection.adapters.frameworks.sglang.window import ObservationWindow
from markov_collection.artifacts import write_json


class NativeServingInstance:
    def __init__(
        self, source: CaptureInput, instance: InstanceConfiguration, output: Path, index: int, rank_offset: int,
        observe_all_workers: bool, resource_trace: bool,
    ) -> None:
        self.model = source.model
        self.observe_all_workers = observe_all_workers
        self.resource_trace = resource_trace
        self.output = output.resolve()
        self.instance = instance
        self.directory = self.output / "instances" / str(index)
        self.rank_offset = rank_offset
        configuration = instance.configuration
        self.configuration = configuration.values
        self.workload = source.workload
        self.submission_groups = source.submission_groups
        self.tp, self.pp, self.ep = configuration.tp, configuration.pp, configuration.ep
        self.roles = tuple(range(rank_offset, rank_offset + self.tp * self.pp))
        self.delivery = {}

    @staticmethod
    def warmup_requests(page_size: int) -> dict[str, Any]:
        lengths = ((max(1, page_size - 1), 4), (page_size + 1, 1), (page_size // 2 + 1, 3), (1, 2))
        return {
            "requests": [
                {"id": str(index), "arrival_ns": 0, "prompt_tokens": prompt, "output_tokens": output}
                for index, (prompt, output) in enumerate(lengths)
            ],
            "prefixes": [],
        }

    def generate(self, engine: Any, phase: str, submissions: EngineSubmissions, clock: Any, barrier: Any) -> None:
        submissions.content.check_vocabulary(engine.tokenizer_manager.model_config.vocab_size)
        print(f"markov: native {phase} begin", flush=True)
        try:
            engine.loop.run_until_complete(submissions.run(engine, clock, barrier, self.output, self.roles))
        finally:

            # Delivery records explain actual ingress delay; declared arrival_ns is never rewritten.
            self.delivery[phase] = submissions.delivery
            write_json(self.directory / "request-delivery.json", self.delivery)
        print(f"markov: native {phase} complete", flush=True)

    def reset(self, engine: Any, phase: str) -> None:

        # End observed serving at native idle before sending the driver's administrative reset.
        ObservationWindow.wait(self.output, self.roles, phase, "drained")
        result = engine.loop.run_until_complete(engine.tokenizer_manager.flush_cache(timeout_s=30))
        if not result.success:
            raise RuntimeError(f"native cache reset failed: {result.message}")
        ObservationWindow.wait(self.output, self.roles, phase, "complete")

    def run(self, barrier, clock, bootstrap, rendezvous) -> dict:
        self.directory.mkdir()
        description = self.directory / "session.json"
        warmup = self.warmup_requests(self.configuration["page_size"])
        mode = self.instance.mode
        warm = EngineSubmissions(warmup, mode)
        capture = EngineSubmissions(self.workload, mode, self.submission_groups)
        warm.prepare("warmup", bootstrap, 1)
        capture.prepare("capture", bootstrap, 1 + len(warmup["requests"]))
        session = {
            "output": str(self.output), "instance_directory": str(self.directory), "mode": mode,
            "rendezvous": {"host": rendezvous["MASTER_ADDR"], "port": int(rendezvous["MASTER_PORT"])},
            "rank_offset": self.rank_offset, "native_participants": self.roles,
            "pp_size": self.pp, "ep_size": self.ep,
            "observe_all_workers": self.observe_all_workers,
            "resource_trace": self.resource_trace,
            "warmup_requests": [f"warmup/{index}" for index in range(len(warmup["requests"]))],
            "source_requests": [f"capture/{index}" for index in range(len(self.workload["requests"]))],
            "protocol_requests": capture.protocol_requests,
            "content": self.workload,
            "submission_groups": self.submission_groups,
            "serving_input": {"configuration": self.configuration, **self.workload},
        }
        write_json(description, session)
        os.environ.update(rendezvous)
        os.environ["MARKOV_GRAPH_SESSION"] = str(description)
        os.environ["MARKOV_NATIVE_RUNTIME_DIR"] = str(self.output / "runtime")
        os.environ["SGLANG_PLUGINS"] = "markov"
        if self.ep > 1:
            os.environ.update(DEEP_USE_ALLTOALL_MODE="1", DEEPEP_HCCL_BUFFSIZE="1024", HCCL_BUFFSIZE="1024")

        from sglang.srt.entrypoints.engine import Engine

        model = self.model
        configuration = self.configuration
        chunk_tokens = configuration["prefill_chunk_tokens"]
        options = {
            "model_path": model["path"], "dtype": model["dtype"], "device": "npu",
            "tp_size": self.tp, "pp_size": self.pp, "base_gpu_id": self.rank_offset,
            "nccl_port": int(rendezvous["MASTER_PORT"]),
            "attention_backend": configuration["attention_backend"],
            "page_size": configuration["page_size"],
            "max_running_requests": configuration["max_running_requests"],
            "max_total_tokens": configuration["max_total_tokens"],
            "max_prefill_tokens": configuration["max_prefill_tokens"],
            "chunked_prefill_size": chunk_tokens if chunk_tokens > 0 else -1,
            "enable_mixed_chunk": configuration["mixed_batches"],
            "disable_radix_cache": not configuration["prefix_cache"],
            "disable_overlap_schedule": True, "disable_cuda_graph": True,
            "skip_tokenizer_init": True, "load_format": "dummy", "sleep_on_idle": True, "trust_remote_code": True,
            "mem_fraction_static": 0.8, "log_level": "warning",
        }
        if mode != "null":
            options.update(
                disaggregation_mode=mode, disaggregation_transfer_backend="ascend",
                disaggregation_bootstrap_port=bootstrap["port"],
                disaggregation_decode_enable_radix_cache=mode == "decode" and configuration["prefix_cache"],
            )
        if "microbatch_requests" in configuration:
            options["pp_max_micro_batch_size"] = configuration["microbatch_requests"]
        if self.ep > 1:
            options.update(ep_size=self.ep, moe_a2a_backend="deepep", deepep_mode="normal")
        from sglang.srt.plugins import load_plugins

        # Native argument normalization can change EP size; reject that change before loading weights or launching workers.
        load_plugins()
        args = Engine.server_args_class(**options)
        if (args.tp_size, args.pp_size, args.ep_size) != (self.tp, self.pp, self.ep):
            raise ValueError("native configuration changed the requested parallel topology")
        started_ns = time.monotonic_ns()
        engine = Engine(server_args=args)
        try:
            initialized_ns = time.monotonic_ns()
            barrier.wait()
            self.generate(engine, "warmup", warm, clock, barrier)
            self.reset(engine, "warmup")
            warmed_ns = time.monotonic_ns()

            barrier.wait()
            engine.start_profile(output_dir=str(self.output / "native"), activities=["CPU", "GPU"], record_shapes=False, with_stack=False)
            ObservationWindow.wait(self.output, self.roles, "profiler", "complete")
            plan = json.loads((self.directory / "role-plan.json").read_text())
            selected = tuple(plan["observed_roles"])
            try:
                barrier.wait()
                self.generate(engine, "capture", capture, clock, barrier)
                self.reset(engine, "source")
                barrier.wait()
            finally:
                engine.stop_profile()
                NativeProfiles.wait(self.output, selected, self.roles)
            captured_ns = time.monotonic_ns()
        finally:
            try:
                engine.shutdown()
            finally:

                # Engine stops its workers but leaves the Prefill bootstrap thread to its owner.
                if engine.tokenizer_manager.bootstrap_server is not None:
                    engine.tokenizer_manager.bootstrap_server.close()
        return {
            "id": self.instance.identity, "mode": mode, "rank_offset": self.rank_offset, "observed_roles": selected,
            "native_participants": self.roles,
            "initialization_ns": initialized_ns - started_ns,
            "warmup_ns": warmed_ns - initialized_ns,
            "capture_ns": captured_ns - warmed_ns,
        }
