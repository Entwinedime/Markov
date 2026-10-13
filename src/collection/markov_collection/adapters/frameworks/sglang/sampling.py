"""Independent CPU or device measurements over a requested batch/length domain."""

from __future__ import annotations

import argparse
import asyncio
import dataclasses
import importlib.metadata
import json
import os
import signal
from pathlib import Path
from typing import Any

from markov_collection.adapters.runtimes.ascend.artifacts import AscendCaptureArtifacts
from markov_collection.adapters.runtimes.ascend.environment import RuntimeEnvironment
from markov_collection.adapters.runtimes.ascend.worker_units import CpuWorkerUnits
from markov_collection.artifacts.json_file import write_json
from markov_collection.artifacts.sampling_domain import SamplingDomain
from markov_collection.instrumentation.session import CaptureSession


class NativeSampling(CaptureSession):
    def run(self) -> None:
        if not self.prepared["permission"]["allow_run"]:
            raise ValueError("performance collection permission is disabled")
        if self.prepared["framework"] != "sglang":
            raise ValueError("independent sampling currently implements SGLang")

        from sglang.srt.entrypoints.engine import Engine

        artifacts = AscendCaptureArtifacts(self.prepared, self.directory)
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
        os.environ["MARKOV_CPU_SAMPLES"] = str(self.directory / "cpu_samples")
        os.environ["MARKOV_OBSERVATIONS"] = str(self.directory / "observations")
        os.environ["SGLANG_PLUGINS"] = "markov"

        engine = Engine(
            model_path=self.prepared["model_path"],
            tokenizer_path=self.prepared["tokenizer_path"],
            skip_tokenizer_init=True,
            **self.prepared["configuration"],
        )
        signal.signal(signal.SIGQUIT, self._handle_worker_failure)
        try:
            if device:
                engine.start_profile(
                    output_dir=str(self.directory / "native_profile"),
                    activities=["CPU", "GPU"],
                    record_shapes=True,
                    with_stack=False,
                )

            try:
                completions = self._measure(engine, grid, repetitions)
            finally:
                if device and not self._worker_failed:
                    engine.stop_profile()

            write_json(
                self.directory / "native_environment.json",
                {
                    "framework": "sglang",
                    "framework_version": importlib.metadata.version("sglang"),
                    "runtime": RuntimeEnvironment().describe(),
                    "cpu_count": os.cpu_count(),
                    "effective_server_args": dataclasses.asdict(engine.tokenizer_manager.server_args),
                    "profiler_enabled": device,
                    "native_api_journal_enabled": device,
                    "native_wait_cpu_sampling_enabled": not device,
                },
            )
        finally:
            engine.shutdown()

        write_json(self.directory / "measurement_completions.json", completions)
        if device:
            artifacts.publish()
            return

        samples = sorted((self.directory / "cpu_samples").glob("*.jsonl"))
        workers = sorted((self.directory / "cpu_workers").glob("*.jsonl"))
        worker_units = self.directory / "cpu_worker_units.json"
        worker_coverage = CpuWorkerUnits(samples, workers).publish(worker_units)
        warmup_coverage, completion_coverage = self._cpu_coverage(samples)
        gaps = [
            {"kind": "cost", "reason": "non_runtime_wait_cpu_not_classified"},
            {"kind": "cost", "reason": "clock_probe_overhead_not_calibrated"},
            {"kind": "cost", "reason": "joint_latency_includes_native_internal_waits"},
        ]
        if warmup_coverage["missing_conditions"]:
            gaps.append({"kind": "cost", "reason": "warmup_execution_domain_incomplete"})
        if completion_coverage["missing_request_ids"]:
            gaps.append({"kind": "observation", "reason": "measurement_completion_observation_incomplete"})

        write_json(
            self.directory / "performance_manifest.json",
            {
                "role": "performance",
                "framework": "sglang",
                "measurement_kind": "native_serving_timing",
                "input": "prepared_input.json",
                "environment": "native_environment.json",
                "completions": "measurement_completions.json",
                "cpu_samples": [str(path.relative_to(self.directory)) for path in samples],
                "cpu_units": [
                    str(path.relative_to(self.directory))
                    for path in sorted((self.directory / "cpu_units").glob("*.jsonl"))
                ],
                "cpu_workers": [str(path.relative_to(self.directory)) for path in workers],
                "cpu_worker_units": worker_units.name,
                "cpu_worker_coverage": worker_coverage,
                "cpu_worker_readiness": "needs_data",
                "cpu_worker_gaps": [
                    "runtime_submission_semantics_not_qualified",
                    "unmeasured_observation_boundary_overhead",
                    "target_operation_matching_not_implemented",
                ],
                "cpu_unit_boundary": "complete native call; phase samples remain inclusive of this work",
                "cpu_unit_readiness": "needs_data",
                "cpu_unit_gaps": [
                    "call_and_clock_boundary_overhead_not_qualified",
                    "independent_cost_validation_missing",
                ],
                "worker_environments": [
                    str(path.relative_to(self.directory))
                    for path in sorted((self.directory / "cpu_samples").glob("*-environment.json"))
                ],
                "clock": {
                    "cpu_active_ns": "per-thread CPU time",
                    "runtime_wait_wall_ns": "existing runtime sync wall time; not service cost",
                    "start_ns/end_ns": "monotonic; not a service cost",
                },
                "completion_boundary": "conservative native synchronization proxy, not an exact hardware end",
                "warmup_coverage": warmup_coverage,
                "completion_coverage": completion_coverage,
                "readiness": "needs_data",
                "gaps": gaps,
            },
        )

    def _measure(self, engine: Any, grid: list[dict[str, Any]], repetitions: int) -> list[dict[str, Any]]:
        completions = []
        for point in grid:
            if not engine.flush_cache():
                raise RuntimeError("independent sampling could not reset the native request/cache state")

            # Observe actual batches; client batch size alone does not prove
            # that the native scheduler warmed the requested execution path.
            self._generate(engine, point, f"markov-measure-warmup/{point['id']}")
            engine.loop.remove_signal_handler(signal.SIGQUIT)
            signal.signal(signal.SIGQUIT, self._handle_worker_failure)

            for repetition in range(repetitions):
                if not engine.flush_cache():
                    raise RuntimeError("independent sampling could not reset the native request/cache state")
                response = self._generate(engine, point, f"markov-measure/{point['id']}/{repetition}")
                completions.append(
                    {
                        "grid": point["id"],
                        "repetition": repetition,
                        "completion_tokens": [item["meta_info"]["completion_tokens"] for item in response],
                    }
                )

        # Generation replies are sent inside native publication. This control RPC
        # reaches the next scheduler iteration, after the observer has persisted
        # its final records, before shutdown can terminate the worker.
        if not engine.flush_cache():
            raise RuntimeError("independent sampling could not acknowledge final publication")

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

    def _cpu_coverage(self, samples: list[Path]) -> tuple[dict[str, Any], dict[str, Any]]:
        domain = SamplingDomain(self.prepared)
        finished: set[str] = set()
        for path in samples:
            with path.open(encoding="utf-8") as stream:
                for line in stream:
                    record = json.loads(line)
                    domain.observe(record["rank"], record["step"], record["metadata"])
                    if record["step"] == "publish":
                        finished.update(record["metadata"]["finished_request_ids"])

        declared = domain.measurement_ids | domain.warmup_ids
        return domain.coverage(), {
            "declared_requests": len(declared),
            "observed_finished_requests": len(finished),
            "missing_request_ids": sorted(declared - finished),
        }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()

    with arguments.input.open(encoding="utf-8") as stream:
        prepared = json.load(stream)

    NativeSampling(prepared, arguments.output).run()


if __name__ == "__main__":
    main()
