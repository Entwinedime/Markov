"""Deliver declared arrivals without waiting for earlier requests to finish."""

from __future__ import annotations

import asyncio
import time
from collections import defaultdict
from typing import Any

from markov_workload import WorkloadContent
from markov_collection.adapters.frameworks.sglang.window import ObservationWindow


class EngineSubmissions:

    def __init__(self, source: dict[str, Any], mode: str, groups: list[list[str]] | None = None) -> None:
        self.mode = mode
        self.requests = source["requests"]
        self.content = WorkloadContent(self.requests, source["prefixes"])
        indices = {request["id"]: index for index, request in enumerate(self.requests)}
        self.groups = []
        for group in groups if groups is not None else [list(indices)]:
            arrivals: dict[int, list[int]] = defaultdict(list)
            for identity in group:
                index = indices[identity]
                arrivals[self.requests[index]["arrival_ns"]].append(index)
            self.groups.append(sorted(arrivals.items()))
        self.delivery: list[dict[str, Any]] = []

    def prepare(self, prefix: str, bootstrap: dict | None, first_room: int) -> None:
        self.prefix = prefix
        self.protocol_requests = {}

        # Construct prompts before observation; each group preserves its own declared arrival offsets.
        prompts = [self.content.prompt(request["id"]) for request in self.requests]
        self.submissions = []
        for group, arrivals in enumerate(self.groups):
            self.submissions.append([])
            for arrival, indices in arrivals:
                payload = {
                    "input_ids": [prompts[index] for index in indices],
                    "sampling_params": [
                        {"max_new_tokens": self.requests[index]["output_tokens"], "temperature": 0, "ignore_eos": True}
                        for index in indices
                    ],
                    "rid": [f"{prefix}/{index}" for index in indices], "stream": True,
                }
                if self.mode != "null":
                    payload.update(
                        bootstrap_host=[bootstrap["host"]] * len(indices),
                        bootstrap_port=[bootstrap["port"]] * len(indices),
                        bootstrap_room=[first_room + index for index in indices],
                    )

                    # Workers can receive remote reservations before dispatching their own copy of the request.
                    self.protocol_requests.update(
                        (str(room), self.requests[index]["id"])
                        for room, index in zip(payload["bootstrap_room"], indices, strict=True)
                    )
                self.submissions[-1].append((payload, {"group": group, "arrival_ns": arrival, "requests": indices}))

    async def run(self, endpoint: Any, clock: Any, barrier: Any, directory, ranks) -> None:
        origin_ns = clock.origin.value
        records = [record for group in self.submissions for _, record in group]
        self.delivery = [{"clock": "monotonic_raw_ns", "origin_ns": origin_ns, "submissions": records}]
        for index, group in enumerate(self.submissions):
            if index:

                # Worker idle, not client completion, closes a group; both PD instances join before advancing time.
                await asyncio.to_thread(ObservationWindow.wait, directory, ranks, "source", f"group/{index - 1}")
                barrier.wait()
            offset_ns = clock.origin.value - origin_ns
            for _, record in group:
                record["scheduled_ns"] = offset_ns + record["arrival_ns"]
            tasks = [asyncio.create_task(self.submit_at(endpoint, payload, record, origin_ns)) for payload, record in group]
            try:
                responses = await asyncio.gather(*tasks)
            except BaseException:
                for task in tasks:
                    task.cancel()
                await asyncio.gather(*tasks, return_exceptions=True)
                raise
            indices = [request for _, record in group for request in record["requests"]]
            self.check_responses(self.prefix, [row for response in responses for row in response], indices)

    async def submit_at(self, endpoint: Any, payload: dict, record: dict, started_ns: int) -> list[dict]:
        deadline_ns = started_ns + record["scheduled_ns"]

        # Event-loop timers may wake early; release only after the shared workload clock reaches arrival.
        while (remaining_ns := deadline_ns - time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)) > 0:
            await asyncio.sleep(remaining_ns / 1e9)
        record["submitted_ns"] = time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW) - started_ns
        return await self.send(endpoint, payload)

    def check_responses(self, prefix: str, rows: list[dict], indices: list[int]) -> None:
        expected = {
            f"{prefix}/{index}": 1 if self.mode == "prefill" else self.requests[index]["output_tokens"]
            for index in indices
        }
        tokens = {
            f"{prefix}/{index}": self.content.output_token(self.requests[index]["id"])
            for index in indices
        }
        completed = set()
        for row in rows:
            metadata = row["meta_info"]
            identity = metadata["id"]
            reason = metadata["finish_reason"]
            if reason is None or reason["type"] != "length" or metadata["completion_tokens"] != expected[identity]:
                raise RuntimeError(f"native request {identity} did not finish its fixed output: {reason}")
            if prefix != "warmup" and any(token != tokens[identity] for token in row["output_ids"]):
                raise RuntimeError(f"native request {identity} did not preserve its declared synthetic output")
            if identity in completed:
                raise RuntimeError(f"native request completed twice: {identity}")
            completed.add(identity)
        if completed != expected.keys():
            raise RuntimeError(f"native responses omitted requests: {sorted(expected.keys() - completed)}")

    @staticmethod
    async def send(endpoint: Any, payload: dict) -> list[dict]:
        rows = []

        # Keep native streaming semantics while retaining only terminal responses.
        async for row in await endpoint.async_generate(**payload):
            if row["meta_info"]["finish_reason"] is not None:
                rows.append(row)
        return rows
