"""Deliver declared arrivals without waiting for earlier requests to finish."""

from __future__ import annotations

import asyncio
import json
import time
from abc import ABC, abstractmethod
from collections import defaultdict
from typing import Any

from markov_workload import WorkloadContent


class NativeSubmissions(ABC):
    roles: tuple[str, ...]

    def __init__(self, source: dict[str, Any], groups: list[list[int]]) -> None:
        self.requests = source["requests"]
        self.content = WorkloadContent(self.requests, source["prefixes"])
        if not all(groups) or sorted(index for group in groups for index in group) != list(range(len(self.requests))):
            raise ValueError("native submissions must contain every request exactly once")
        for request in self.requests:
            arrival = request["arrival_ns"]
            if type(arrival) is not int or not 0 <= arrival <= (1 << 63) - 1:
                raise ValueError("native arrival_ns must be a nonnegative signed 64-bit integer")

        # Discovery rounds may prime caches; each round has its own workload time origin.
        self.rounds = []
        for group in groups:
            arrivals: dict[int, list[int]] = defaultdict(list)
            for index in sorted(group):
                arrivals[self.requests[index]["arrival_ns"]].append(index)
            self.rounds.append(sorted(arrivals.items()))
        self.delivery: list[dict[str, Any]] = []

    async def run(self, endpoint: Any, prefix: str) -> dict[str, list[dict[str, Any]]]:

        # Materialize inputs before the clock starts; prompt construction is not arrival delay.
        prompts = [self.content.prompt(request["id"]) for request in self.requests]
        responses = {role: [] for role in self.roles}
        self.delivery = []
        for arrivals in self.rounds:
            submissions = []
            for arrival, indices in arrivals:
                payload = {
                    "input_ids": [prompts[index] for index in indices],
                    "sampling_params": [
                        {"max_new_tokens": self.requests[index]["output_tokens"], "temperature": 0, "ignore_eos": True}
                        for index in indices
                    ],
                    "rid": [f"{prefix}/{index}" for index in indices],
                    "stream": True,
                }
                submissions.append((payload, {"arrival_ns": arrival, "requests": indices}))


            # Semantic scopes use this same process-independent performance clock on the serving host.
            started_ns = time.perf_counter_ns()
            self.delivery.append({"origin_ns": started_ns, "submissions": [record for _, record in submissions]})
            tasks = [
                asyncio.create_task(self.submit_at(endpoint, payload, record, started_ns))
                for payload, record in submissions
            ]
            try:
                results = await asyncio.gather(*tasks)
            except BaseException:

                # Failed or cancelled runs must not keep submitting requests in a later phase.
                for task in tasks:
                    task.cancel()
                await asyncio.gather(*tasks, return_exceptions=True)
                raise

            indices = [index for _, group in arrivals for index in group]
            for role in self.roles:
                rows = [row for result in results for row in result[role]]
                self.check_responses(role, prefix, rows, indices)
                responses[role].extend(rows)
        return responses

    async def submit_at(self, endpoint: Any, payload: dict, record: dict, started_ns: int) -> dict:
        await asyncio.sleep(max(0, started_ns + record["arrival_ns"] - time.perf_counter_ns()) / 1e9)
        record["submitted_ns"] = time.perf_counter_ns() - started_ns
        return await self.send(endpoint, payload, record["requests"])

    def check_responses(self, role: str, prefix: str, rows: list[dict], indices: list[int]) -> None:
        expected = {
            f"{prefix}/{index}": 1 if role == "prefill" else self.requests[index]["output_tokens"]
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

    @abstractmethod
    async def send(self, endpoint: Any, payload: dict, indices: list[int]) -> dict:
        pass


class EngineSubmissions(NativeSubmissions):
    roles = ("model",)

    async def send(self, endpoint: Any, payload: dict, indices: list[int]) -> dict:
        rows = []

        # Keep native streaming semantics while retaining only terminal responses.
        async for row in await endpoint.async_generate(**payload):
            if row["meta_info"]["finish_reason"] is not None:
                rows.append(row)
        return {"model": rows}


class DisaggregatedSubmissions(NativeSubmissions):
    roles = ("prefill", "decode")

    async def send(self, endpoint: Any, payload: dict, indices: list[int]) -> dict:
        session, ports, bootstrap_port, room_begin = endpoint
        payload.update(
            bootstrap_host=["127.0.0.1"] * len(indices),
            bootstrap_port=[bootstrap_port] * len(indices),
            bootstrap_room=[room_begin + index for index in indices],
        )
        tasks = [asyncio.create_task(self.generate(session, port, payload)) for port in ports]
        try:
            return dict(zip(self.roles, await asyncio.gather(*tasks), strict=True))
        except BaseException:
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            raise

    @staticmethod
    async def generate(session: Any, port: int, payload: dict) -> list[dict]:
        rows = []
        async with session.post(f"http://127.0.0.1:{port}/generate", json=payload) as response:
            response.raise_for_status()

            # Token arrays may exceed aiohttp's readline limit; keep only the unfinished SSE line.
            pending = b""
            async for chunk in response.content.iter_any():
                *lines, pending = (pending + chunk).split(b"\n")
                for line in lines:
                    if not line.startswith(b"data: ") or line.strip() == b"data: [DONE]":
                        continue
                    row = json.loads(line[6:])
                    if row["meta_info"]["finish_reason"] is not None:
                        rows.append(row)
        return rows
