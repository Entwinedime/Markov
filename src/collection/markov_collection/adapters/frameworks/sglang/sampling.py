"""Independent device and host measurements over requested native batch work."""

from __future__ import annotations

import dataclasses
import importlib.metadata
import os
import signal
import time
from pathlib import Path
from typing import Any

from markov_collection.adapters.frameworks.sglang.session import SGLangSession
from markov_collection.adapters.runtimes.ascend.cpu_boundaries import AscendCpuBoundaries
from markov_collection.adapters.runtimes.ascend.environment import RuntimeEnvironment
from markov_collection.adapters.runtimes.ascend.transfer_sampling import TransferMeasurements
from markov_collection.artifacts.cpu_measurements import CpuMeasurements
from markov_collection.artifacts.json_file import write_json
from markov_collection.artifacts.sampling_domain import SamplingDomain


class SGLangSampling(SGLangSession):
    def prepare(self) -> None:
        grid = self.prepared["measurement_grid"]
        repetitions = self.prepared["repetitions"]
        self.prepared["measurement_ids"] = [
            f"markov-measure/{point['id']}/{repetition}/{slot}"
            for point in grid
            for repetition in range(repetitions)
            for slot in range(len(point["requests"]))
        ]
        self.prepared["warmup_ids"] = [
            f"markov-measure-warmup/{point['id']}/{slot}" for point in grid for slot in range(len(point["requests"]))
        ]
        self.prepared["control_ids"] = (
            [
                f"markov-control/{point['id']}/{repetition}/{slot}"
                for point in grid
                for repetition in range(repetitions)
                for slot in range(len(point["requests"]))
            ]
            if self.prepared["measurement_kind"] == "cpu"
            else []
        )

        os.environ["MARKOV_TRANSFER_SCOPES"] = str(self.directory / "transfer_scopes")
        super().prepare()

    def run(self) -> None:
        from sglang.srt.entrypoints.engine import Engine

        grid = self.prepared["measurement_grid"]
        repetitions = self.prepared["repetitions"]
        device = self.prepared["measurement_kind"] == "device"
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
            warmup = self._warmup(engine)
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
            windows = []
            for item in completions:
                windows.append(item["window"])
                if "inactive_recorder_window" in item:
                    windows.append(item["inactive_recorder_window"])
            steady_state = self._steady_windows(windows, warmup)

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
                    "cpu_activity_sampling_enabled": self.prepared["measurement_kind"] == "cpu",
                    "semantic_instrumentation_enabled": False,
                    "steady_state": steady_state,
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
        transfers = TransferMeasurements(scopes, workers, domain)
        coverage = transfers.publish(worker_units)
        completion_coverage = domain.completion_coverage()
        warmup_coverage = domain.coverage()
        feature_coverage = domain.feature_coverage()
        gaps = []
        if steady_state["status"] != "steady":
            gaps.append({"kind": "observation", "reason": steady_state["status"]})
        if warmup_coverage["missing_conditions"]:
            gaps.append({"kind": "cost", "reason": "warmup_execution_domain_incomplete"})
        if completion_coverage["missing_request_ids"]:
            gaps.append({"kind": "observation", "reason": "measurement_completion_observation_incomplete"})
        if feature_coverage["missing_point_indices"]:
            gaps.append({"kind": "cost", "reason": "requested_batch_work_not_observed"})

        manifest = {
            "role": "performance",
            "framework": "sglang",
            "measurement_kind": "native_serving_timing",
            "input": "prepared_input.json",
            "environment": "native_environment.json",
            "completions": "measurement_completions.json",
            "transfer_scopes": [str(path.relative_to(self.directory)) for path in scopes],
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
        }
        cpu_measurements = None
        if self.prepared["measurement_kind"] == "cpu":
            cpu_measurements = self._publish_cpu(manifest, scopes, domain, completions)
        write_json(self.directory / "performance_manifest.json", manifest)
        transfers.consume_journals()
        if cpu_measurements is not None and manifest["readiness"] == "ready":
            cpu_measurements.consume_journals()

    def _publish_cpu(
        self,
        manifest: dict[str, Any],
        scopes: list[Path],
        domain: SamplingDomain,
        completions: list[dict[str, Any]],
    ) -> CpuMeasurements:
        summary_path = self.directory / "cpu_measurements.json"
        measurements = CpuMeasurements(
            self.directory / "cpu_activities",
            scopes,
            domain,
            AscendCpuBoundaries(),
            self.prepared["cpu_services"],
            self.prepared["cpu_service_descriptors"],
        )
        summary = measurements.publish(summary_path, completions)
        manifest.update(
            {
                "measurement_kind": "native_cpu_activity",
                "cpu_activities": [str(path.relative_to(self.directory)) for path in measurements.paths],
                "cpu_measurements": summary_path.name,
                "readiness": "needs_data",
                "clock": {
                    "wall_ns": "CLOCK_MONOTONIC at native activity boundaries",
                    "thread_cpu_ns": "CLOCK_THREAD_CPUTIME_ID with marked observer CPU excluded",
                    "observer_cpu_ns": "cumulative excluded CPU on this thread",
                    "observer_cycles": "completed outer observer intervals on this thread",
                    "service_cpu_ns": "exclusive service CPU minus lane-calibrated clock-read cost per observer cycle",
                },
            }
        )
        if "service_units" in summary:
            manifest["cpu_service_units"] = summary["service_units"]
            manifest["cpu_service_readiness"] = summary["cost_readiness"]
        if summary["cost_gap"] is not None:
            manifest["gaps"].append({"kind": "cost", "reason": summary["cost_gap"]})
        manifest["gaps"].extend(summary["gaps"])
        manifest["readiness"] = "needs_data" if manifest["gaps"] else "ready"
        return measurements

    def _measure(self, engine: Any, grid: list[dict[str, Any]], repetitions: int) -> list[dict[str, Any]]:
        completions = []
        for point in grid:
            # Observe actual batches; client batch size alone does not prove
            # that the native scheduler warmed the requested execution path.
            self._cohorts.generate(point["requests"], f"markov-measure-warmup/{point['id']}")
            self._reset(engine)

            for repetition in range(repetitions):
                control = None
                control_identity = f"markov-control/{point['id']}/{repetition}"
                if self.prepared["measurement_kind"] == "cpu":
                    # Undeclared IDs pass through native serving without active
                    # cost scopes. The global callback remains installed: this
                    # pair measures active-recorder perturbation, not its full
                    # difference from a process without callbacks.
                    control_started_ns = time.perf_counter_ns()
                    self._cohorts.generate(point["requests"], control_identity)
                    self._reset(engine)
                    control = {"started_ns": control_started_ns, "ended_ns": time.perf_counter_ns()}

                started_ns = time.perf_counter_ns()
                identity = f"markov-measure/{point['id']}/{repetition}"
                response = self._cohorts.generate(point["requests"], identity)
                # A client response can precede the final native publication.
                # Qualification encloses that completion; costs keep their own scopes.
                self._reset(engine)
                ended_ns = time.perf_counter_ns()
                completions.append(
                    {
                        "grid": point["id"],
                        "repetition": repetition,
                        "window": {"started_ns": started_ns, "ended_ns": ended_ns},
                        "completion_tokens": [item["meta_info"]["completion_tokens"] for item in response],
                        "request_ids": [f"{identity}/{slot}" for slot in range(len(point["requests"]))],
                        **(
                            {
                                "inactive_recorder_window": control,
                                "inactive_request_ids": [
                                    f"{control_identity}/{slot}" for slot in range(len(point["requests"]))
                                ],
                            }
                            if control is not None
                            else {}
                        ),
                    }
                )
        return completions
