"""SGLang engine launch, representative request resolution and submission."""

from __future__ import annotations

import asyncio
import dataclasses
import importlib.metadata
import json
import os
import signal
import time
from typing import Any

from markov_collection.adapters.runtimes.ascend.environment import RuntimeEnvironment
from markov_collection.artifacts.json_file import write_json
from markov_collection.instrumentation.session import CaptureSession


class SGLangCapture(CaptureSession):
    def run(self) -> None:
        from sglang.srt.entrypoints.engine import Engine

        requests = self.prepared["requests"]
        os.environ["SGLANG_PLUGINS"] = "markov"

        engine = Engine(
            model_path=self.prepared["model_path"],
            tokenizer_path=self.prepared["tokenizer_path"],
            skip_tokenizer_init=True,
            **self.prepared["configuration"],
        )
        signal.signal(signal.SIGQUIT, self._handle_worker_failure)
        try:
            if "representative_recipe" in self.prepared:
                self._resolve_representative_requests(engine)

            # One prefill and one decode establish a workload-independent start.
            # With radix cache disabled, normal completion releases request KV;
            # do not flush the device allocator and discard warmed resources.
            warmup_id = "markov-warmup"
            request_ids = {request["id"] for request in requests}
            while warmup_id in request_ids:
                warmup_id += "_"

            warmup_input = [1]
            warmup_sampling = {"max_new_tokens": 2, "ignore_eos": True, "temperature": 0}
            engine.generate(
                input_ids=warmup_input,
                sampling_params=warmup_sampling,
                rid=warmup_id,
            )

            # Native tokenizer setup installs an asyncio SIGQUIT handler during warmup.
            # A failed worker must unwind this capture instead of leaving stop_profile
            # awaiting a response from that dead worker until the full budget expires.
            engine.loop.remove_signal_handler(signal.SIGQUIT)
            signal.signal(signal.SIGQUIT, self._handle_worker_failure)

            engine.start_profile(
                output_dir=str(self.directory / "native_profile"),
                activities=["CPU", "GPU"],
                record_shapes=True,
                with_stack=False,
            )
            try:
                responses = engine.loop.run_until_complete(self._execute(engine))
                cleanup = []
                if self.prepared["role"] == "base":
                    cleanup = engine.loop.run_until_complete(self._await_cleanup(request_ids))
            finally:
                if not self._worker_failed:
                    engine.stop_profile()

            write_json(self.directory / "responses.json", responses)
            write_json(
                self.directory / "native_environment.json",
                {
                    "framework": "sglang",
                    "framework_version": importlib.metadata.version("sglang"),
                    "runtime": RuntimeEnvironment().describe(),
                    "configuration": self.prepared["configuration"],
                    "model_configuration": self._model_configuration(),
                    "effective_server_args": dataclasses.asdict(engine.tokenizer_manager.server_args),
                    "collection_completion": cleanup,
                    "initialization": {
                        "engine": "fresh_process",
                        "warmup": {
                            "input_tokens": len(warmup_input),
                            "input_ids": warmup_input,
                            "sampling_parameters": warmup_sampling,
                        },
                        "workspace_snapshot": "after_warmup_before_profile",
                    },
                },
            )
        finally:
            engine.shutdown()

    async def _await_cleanup(self, request_ids: set[str]) -> list[dict[str, Any]]:
        paths = [
            self.directory / "observations" / f"rank-{rank}-cleanup.json"
            for rank in range(self.prepared["configuration"]["tp_size"])
        ]
        # The outer attempt owns the deadline and terminates this process on
        # budget exhaustion; polling cadence never stands in for completion.
        while True:
            if all(path.exists() for path in paths):
                records = [json.loads(path.read_text(encoding="utf-8")) for path in paths]
                if all(request_ids.issubset(record["request_ids"]) for record in records):
                    return records
            await asyncio.sleep(0.01)

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
        # Pretokenized requests in one native batch reach the scheduler in one
        # message. Separate submissions can split a logical arrival cohort.
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

    async def _execute_groups(self, engine: Any) -> list[dict[str, Any]]:
        origin_ns = time.perf_counter_ns()
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
            completions.append(await self._await_cleanup(completed_ids))

        write_json(self.directory / "group_completions.json", completions)
        write_json(self.directory / "prepared_input.json", self.prepared)
        return results

    async def _execute(self, engine: Any) -> list[dict[str, Any]]:
        if "submission_groups" in self.prepared:
            return await self._execute_groups(engine)

        arrivals: dict[int, list[dict[str, Any]]] = {}
        for request in self.prepared["requests"]:
            arrivals.setdefault(request["arrival_time_ns"], []).append(request)

        origin_ns = time.perf_counter_ns()
        results: list[dict[str, Any]] = []

        async def submit(arrival_ns: int, requests: list[dict[str, Any]]) -> None:
            delay_ns = origin_ns + arrival_ns - time.perf_counter_ns()
            if delay_ns > 0:
                await asyncio.sleep(delay_ns / 1_000_000_000)

            # Different arrival cohorts remain concurrent. Waiting for one
            # cohort's response must not delay a later scheduled arrival.
            results.extend(await self._submit_group(engine, requests))

        await asyncio.gather(*(submit(arrival, requests) for arrival, requests in sorted(arrivals.items())))
        return results
