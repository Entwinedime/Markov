"""Independent device or synchronous-transfer measurements over requested batch work."""

from __future__ import annotations

import asyncio
import dataclasses
import importlib.metadata
import os
import signal
import time
from typing import Any

from markov_collection.adapters.frameworks.sglang.session import SGLangSession
from markov_collection.adapters.runtimes.ascend.environment import RuntimeEnvironment
from markov_collection.adapters.runtimes.ascend.transfer_sampling import TransferMeasurements
from markov_collection.artifacts.json_file import write_json
from markov_collection.artifacts.sampling_domain import SamplingDomain


class SGLangSampling(SGLangSession):
    def run(self) -> None:
        from sglang.srt.entrypoints.engine import Engine

        grid = self.prepared["measurement_grid"]
        repetitions = self.prepared["repetitions"]
        device = self.prepared["measurement_kind"] == "device"
        self.prepared["measurement_ids"] = [
            f"markov-measure/{point['id']}/{repetition}/{slot}"
            for point in grid
            for repetition in range(repetitions)
            for slot in range(len(point["requests"]))
        ]
        self.prepared["warmup_ids"] = [
            f"markov-measure-warmup/{point['id']}/{slot}" for point in grid for slot in range(len(point["requests"]))
        ]

        write_json(self.directory / "prepared_input.json", self.prepared)
        os.environ["MARKOV_PREPARED_INPUT"] = str(self.directory / "prepared_input.json")
        os.environ["MARKOV_TRANSFER_SCOPES"] = str(self.directory / "transfer_scopes")
        os.environ["MARKOV_OBSERVATIONS"] = str(self.directory / "observations")
        os.environ["SGLANG_PLUGINS"] = "markov"

        engine_started_ns = time.perf_counter_ns()
        engine = Engine(
            model_path=self.prepared["model_path"],
            tokenizer_path=self.prepared["tokenizer_path"],
            skip_tokenizer_init=True,
            **self.prepared["configuration"],
        )
        engine_ready_ns = time.perf_counter_ns()
        signal.signal(signal.SIGQUIT, self._handle_worker_failure)
        try:
            warmup_started_ns = time.perf_counter_ns()
            warmup = self._warmup(engine, set(self.prepared["measurement_ids"]) | set(self.prepared["warmup_ids"]))
            warmup_finished_ns = time.perf_counter_ns()
            if device:
                engine.start_profile(
                    output_dir=str(self.directory / "native_profile"),
                    activities=["CPU", "GPU"],
                    record_shapes=True,
                    with_stack=False,
                )

            observation_started_ns = time.perf_counter_ns()
            try:
                completions = self._measure(engine, grid, repetitions)
            finally:
                observation_finished_ns = time.perf_counter_ns()
                if device and not self._worker_failed:
                    engine.stop_profile()
            profile_finished_ns = time.perf_counter_ns()

            write_json(
                self.directory / "native_environment.json",
                {
                    "framework": "sglang",
                    "framework_version": importlib.metadata.version("sglang"),
                    "runtime": RuntimeEnvironment().describe(),
                    "effective_server_args": dataclasses.asdict(engine.tokenizer_manager.server_args),
                    "profiler_enabled": device,
                    "native_api_journal_enabled": device,
                    "synchronous_transfer_sampling_enabled": not device,
                    "semantic_instrumentation_enabled": False,
                    "collection_wall_time_ns": {
                        "engine_initialization": engine_ready_ns - engine_started_ns,
                        "warmup": warmup_finished_ns - warmup_started_ns,
                        "profile_start": observation_started_ns - warmup_finished_ns,
                        "point_warmup_and_measurement": observation_finished_ns - observation_started_ns,
                        "profile_stop_and_export": profile_finished_ns - observation_finished_ns,
                    },
                    "initialization": {"engine": "fresh_process", "warmup": warmup},
                },
            )
        finally:
            engine.shutdown()

        write_json(self.directory / "measurement_completions.json", completions)
        if device:
            return

        scopes = sorted((self.directory / "transfer_scopes").glob("*.jsonl"))
        workers = sorted((self.directory / "cpu_workers").glob("*.jsonl"))
        domain = SamplingDomain(self.prepared)
        worker_units = self.directory / "cpu_worker_units.json.gz"
        coverage = TransferMeasurements(scopes, workers, domain).publish(worker_units)
        completion_coverage = domain.completion_coverage()
        warmup_coverage = domain.coverage()
        feature_coverage = domain.feature_coverage(transfer_executions=True)
        gaps = []
        if warmup_coverage["missing_conditions"]:
            gaps.append({"kind": "cost", "reason": "warmup_execution_domain_incomplete"})
        if completion_coverage["missing_request_ids"]:
            gaps.append({"kind": "observation", "reason": "measurement_completion_observation_incomplete"})
        if feature_coverage["missing_point_indices"]:
            gaps.append({"kind": "cost", "reason": "requested_batch_work_not_observed"})

        write_json(
            self.directory / "performance_manifest.json",
            {
                "role": "performance",
                "framework": "sglang",
                "measurement_kind": "native_serving_timing",
                "input": "prepared_input.json",
                "environment": "native_environment.json",
                "completions": "measurement_completions.json",
                "transfer_scopes": [str(path.relative_to(self.directory)) for path in scopes],
                "cpu_workers": [str(path.relative_to(self.directory)) for path in workers],
                "cpu_worker_units": worker_units.name,
                "cpu_worker_coverage": coverage,
                "cpu_worker_readiness": "needs_data" if coverage["gaps"] else "ready",
                "cpu_worker_gaps": coverage["gaps"],
                "worker_environments": [
                    str(path.relative_to(self.directory))
                    for path in sorted((self.directory / "transfer_scopes").glob("*-environment.json"))
                ],
                "clock": {"wall_ns": "monotonic synchronous driver entry and return"},
                "warmup_coverage": warmup_coverage,
                "feature_coverage": feature_coverage,
                "completion_coverage": completion_coverage,
                "readiness": "needs_data" if gaps or coverage["gaps"] else "ready",
                "gaps": gaps,
            },
        )

    def _measure(self, engine: Any, grid: list[dict[str, Any]], repetitions: int) -> list[dict[str, Any]]:
        completions = []
        for point in grid:
            # Observe actual batches; client batch size alone does not prove
            # that the native scheduler warmed the requested execution path.
            self._generate(engine, point, f"markov-measure-warmup/{point['id']}")

            for repetition in range(repetitions):
                self._reset(engine)
                response = self._generate(engine, point, f"markov-measure/{point['id']}/{repetition}")
                completions.append(
                    {
                        "grid": point["id"],
                        "repetition": repetition,
                        "completion_tokens": [item["meta_info"]["completion_tokens"] for item in response],
                    }
                )

            self._reset(engine)

        return completions

    def _generate(self, engine: Any, point: dict[str, Any], identity: str) -> list[dict[str, Any]]:
        requests = point["requests"]
        if any(request["release_after_tokens"] for request in requests):
            return engine.loop.run_until_complete(self._staggered(engine, requests, identity))

        return engine.generate(
            input_ids=[request["input_tokens"] for request in requests],
            sampling_params=[
                {"max_new_tokens": request["output_tokens"], "ignore_eos": True, "temperature": 0}
                for request in requests
            ],
            rid=[f"{identity}/{slot}" for slot in range(len(requests))],
        )

    async def _staggered(self, engine: Any, requests: list[dict[str, Any]], identity: str) -> list[dict[str, Any]]:
        groups: dict[int, list[int]] = {}
        for slot, request in enumerate(requests):
            groups.setdefault(request["release_after_tokens"], []).append(slot)

        # Gates follow observed token progress, rather than assumed execution delays.
        gates = {offset: asyncio.Event() for offset in groups}
        gates[0].set()
        responses: dict[str, dict[str, Any]] = {}

        await asyncio.gather(
            *[
                self._generate_group(engine, requests, identity, slots, offset, gates, responses)
                for offset, slots in groups.items()
            ]
        )

        return [responses[f"{identity}/{slot}"] for slot in range(len(requests))]

    async def _generate_group(
        self,
        engine: Any,
        requests: list[dict[str, Any]],
        identity: str,
        slots: list[int],
        offset: int,
        gates: dict[int, asyncio.Event],
        responses: dict[str, dict[str, Any]],
    ) -> None:
        await gates[offset].wait()
        stream = await engine.async_generate(
            input_ids=[requests[slot]["input_tokens"] for slot in slots],
            sampling_params=[
                {"max_new_tokens": requests[slot]["output_tokens"], "ignore_eos": True, "temperature": 0}
                for slot in slots
            ],
            rid=[f"{identity}/{slot}" for slot in slots],
            stream=True,
        )

        async for response in stream:
            meta = response["meta_info"]
            responses[meta["id"]] = response
            if meta["id"] == f"{identity}/0":
                for release, gate in gates.items():
                    if meta["completion_tokens"] >= release:
                        gate.set()
