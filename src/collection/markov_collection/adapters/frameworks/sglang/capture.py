"""SGLang engine launch, representative request resolution and submission."""

from __future__ import annotations

import asyncio
import dataclasses
import importlib.metadata
import os
import signal
import time
from typing import Any

from markov_collection.adapters.frameworks.sglang.session import SGLangSession
from markov_collection.artifacts.json_file import write_json


class SGLangCapture(SGLangSession):
    def run(self) -> None:
        requests = self.prepared["requests"]
        profiling = self.prepared["role"] != "validation" or self.prepared["observation_purpose"] == "structure"
        os.environ["SGLANG_PLUGINS"] = "markov"

        # Host wall times describe collection overhead, never target node costs.
        engine_started_ns = time.perf_counter_ns()
        engine = self._start_engine()
        engine_ready_ns = time.perf_counter_ns()
        signal.signal(signal.SIGQUIT, self._handle_worker_failure)
        try:
            if "representative_recipe" in self.prepared:
                self._resolve_representative_requests(engine)

            request_ids = {request["id"] for request in requests}
            warmup_started_ns = time.perf_counter_ns()
            warmup = self._warmup(engine)
            warmup_finished_ns = time.perf_counter_ns()

            profile_start_ns = time.perf_counter_ns()
            if profiling:
                engine.start_profile(
                    output_dir=str(self.directory / "native_profile"),
                    activities=["CPU", "GPU"],
                    record_shapes=True,
                    with_stack=False,
                )
            observation_started_ns = time.perf_counter_ns()
            try:
                request_timing, responses = engine.loop.run_until_complete(self._execute(engine))
                cleanup = []
                if self.prepared["role"] == "base":
                    cleanup = engine.loop.run_until_complete(
                        self._await_cleanup(request_ids, self.directory / "observations")
                    )
            finally:
                observation_finished_ns = time.perf_counter_ns()
                if not self._worker_failed:
                    if profiling:
                        engine.stop_profile()
                    else:
                        # Even a negative cache-reset reply acknowledges the next
                        # worker iteration. The observed port files, not a reset,
                        # establish publication completeness after the window.
                        engine.flush_cache()
            profile_finished_ns = time.perf_counter_ns()

            wall_times = {
                "engine_initialization": engine_ready_ns - engine_started_ns,
                "request_preparation": warmup_started_ns - engine_ready_ns,
                "warmup": warmup_finished_ns - warmup_started_ns,
                "post_warmup_setup": profile_start_ns - warmup_finished_ns,
                "observation_and_cleanup": observation_finished_ns - observation_started_ns,
            }
            wall_times["profile_start" if profiling else "port_observer_setup"] = (
                observation_started_ns - profile_start_ns
            )
            wall_times["profile_stop_and_export" if profiling else "final_publication_acknowledgement"] = (
                profile_finished_ns - observation_finished_ns
            )

            write_json(self.directory / "responses.json", responses)
            write_json(
                self.directory / "native_environment.json",
                {
                    "framework": "sglang",
                    "framework_version": importlib.metadata.version("sglang"),
                    "runtime": self.runtime_environment,
                    "configuration": self.prepared["configuration"],
                    "model_configuration": self.prepared["model_configuration"],
                    "effective_server_args": dataclasses.asdict(engine.tokenizer_manager.server_args),
                    "collection_completion": cleanup,
                    "profiler_enabled": profiling,
                    "semantic_instrumentation_enabled": profiling,
                    "native_api_journal_enabled": profiling,
                    "target_port_observation_enabled": not profiling,
                    # The driver origin defines planned arrivals, not admission
                    # into a modeled block or a directly measured service cost.
                    "request_timing": request_timing,
                    "steady_state": self._steady_windows(
                        [
                            {
                                "started_ns": observation_started_ns,
                                "ended_ns": observation_finished_ns if profiling else profile_finished_ns,
                            }
                        ],
                        warmup,
                    ),
                    "collection_wall_time_ns": wall_times,
                    "initialization": {
                        "engine": "fresh_process",
                        "warmup": warmup,
                        "workspace_snapshot": "after_warmup_before_profile" if profiling else "not_observed",
                    },
                },
            )
        finally:
            engine.shutdown()

        if not profiling:
            self._publish_ports()

    def _publish_ports(self) -> None:
        write_json(
            self.directory / "native_capture.json",
            {
                "role": "validation",
                "framework": "sglang",
                "observation_purpose": "ports",
                "candidate_id": self.prepared["candidate_id"],
                "input": "prepared_input.json",
                "environment": "native_environment.json",
                "responses": "responses.json",
                "expected_ranks": list(self.worker_ranks),
                "ports": [
                    str(path.relative_to(self.directory))
                    for path in sorted((self.directory / "observations").glob("rank-*-ports.json"))
                ],
                "boundaries": {
                    "arrival": "Scheduler.handle_generate_request ScopeReturn; native request ready for scheduling",
                    "submitted": "Scheduler.run_batch ScopeReturn; not device completion",
                    "result_processed": (
                        "Scheduler.process_batch_result ScopeReturn; internal generation and control progress"
                    ),
                    "request_output": (
                        "SchedulerOutputStreamer.stream_output ScopeReturn on a sending role; "
                        "only requests with a positive native send_token_offset increment; "
                        "token handoff to detokenizer, excluding downstream processing and HTTP"
                    ),
                    "observation": (
                        "timestamp immediately after native return, before metadata or journal writing; "
                        "not profiler marker.end"
                    ),
                    "native_call": (
                        "coarse run_batch/process_batch_result wall and control-thread CPU brackets; "
                        "includes CPU consumed by waits/transfers and clock boundary overhead; "
                        "independent validation diagnostics, not cost samples"
                    ),
                },
            },
        )

    def _resolve_representative_requests(self, engine: Any) -> None:
        recipe = self.prepared["representative_recipe"]
        actual = engine.tokenizer_manager.server_args
        page_size = actual.page_size
        if page_size < recipe["minimum_page_size_tokens"]:
            raise ValueError("native page size does not satisfy the representative recipe")

        for field, value in (
            ("minimum_prefill_budget", actual.max_prefill_tokens),
            ("minimum_total_token_capacity", actual.max_total_tokens),
        ):
            bound = recipe[field]
            if value < bound["page_size_multiple"] * page_size + bound["offset_tokens"]:
                raise ValueError(f"native {field} does not satisfy the representative recipe")

        # Stable identities are installed before worker startup. Only prompt
        # geometry is resolved here; synthetic dense prompts contain token 1.
        for request in self.prepared["requests"]:
            length = page_size + request.pop("prompt_page_offset")
            request["input_tokens"] = [1] * length
        recipe["resolved_page_size_tokens"] = page_size
        write_json(self.directory / "prepared_input.json", self.prepared)

    async def _submit_group(self, engine: Any, requests: list[dict[str, Any]]) -> list[dict[str, Any]]:
        # This pretokenized path sends one arrival cohort in a scheduler message.
        # Native admission can still split that message into multiple batches.
        submitted_ns = time.perf_counter_ns()
        responses = await engine.async_generate(
            input_ids=[request["input_tokens"] for request in requests],
            sampling_params=[
                {"max_new_tokens": request["output_tokens"], "ignore_eos": True, "temperature": 0}
                for request in requests
            ],
            rid=[request["id"] for request in requests],
        )
        returned_ns = time.perf_counter_ns()
        return [
            {
                "id": request["id"],
                "submitted_ns": submitted_ns,
                "returned_ns": returned_ns,
                "completion_basis": "batch_response",
                "response": response,
            }
            for request, response in zip(requests, responses, strict=True)
        ]

    @staticmethod
    def _request_origin() -> dict[str, Any]:
        # Profiler markers use MONOTONIC_RAW. Bracket the scheduling clock's
        # origin instead of treating the two clock domains as interchangeable.
        raw_before_ns = time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)
        origin_ns = time.perf_counter_ns()
        raw_after_ns = time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)

        return {
            "clock": "perf_counter_ns",
            "clock_implementation": time.get_clock_info("perf_counter").implementation,
            "origin_ns": origin_ns,
            "raw_clock": "CLOCK_MONOTONIC_RAW",
            "raw_before_ns": raw_before_ns,
            "raw_after_ns": raw_after_ns,
        }

    async def _execute_groups(self, engine: Any) -> tuple[dict[str, Any], list[dict[str, Any]]]:
        request_timing = self._request_origin()
        origin_ns = request_timing["origin_ns"]
        results: list[dict[str, Any]] = []
        completed_ids: set[str] = set()
        completions = []
        for indices in self.prepared["submission_groups"]:
            requests = [self.prepared["requests"][index] for index in indices]

            # Recipe groups have completion barriers; their arrival times are
            # observed, unlike the open arrivals in an ordinary workload.
            group = await self._submit_group(engine, requests)
            for request, response in zip(requests, group, strict=True):
                request["arrival_time_ns"] = response["submitted_ns"] - origin_ns
            results.extend(group)

            # A response proves publication, not removal from running_batch.
            # Cumulative identities prevent a previous group's ack satisfying this barrier.
            completed_ids.update(request["id"] for request in requests)
            completions.append(await self._await_cleanup(completed_ids, self.directory / "observations"))

        write_json(self.directory / "group_completions.json", completions)
        write_json(self.directory / "prepared_input.json", self.prepared)
        return request_timing, results

    async def _execute(self, engine: Any) -> tuple[dict[str, Any], list[dict[str, Any]]]:
        if "submission_groups" in self.prepared:
            return await self._execute_groups(engine)

        arrivals: dict[int, list[dict[str, Any]]] = {}
        for request in self.prepared["requests"]:
            arrivals.setdefault(request["arrival_time_ns"], []).append(request)

        request_timing = self._request_origin()
        origin_ns = request_timing["origin_ns"]
        results: list[dict[str, Any]] = []

        async def submit(arrival_ns: int, requests: list[dict[str, Any]]) -> None:
            delay_ns = origin_ns + arrival_ns - time.perf_counter_ns()
            if delay_ns > 0:
                await asyncio.sleep(delay_ns / 1_000_000_000)

            # Different arrival cohorts remain concurrent. Waiting for one
            # cohort's response must not delay a later scheduled arrival.
            results.extend(await self._submit_group(engine, requests))

        await asyncio.gather(*(submit(arrival, requests) for arrival, requests in sorted(arrivals.items())))
        return request_timing, results
