"""Run native request cohorts with token-progress gates and framework-owned state."""

from __future__ import annotations

import asyncio
from typing import Any


class SGLangRequestCohorts:
    def __init__(self, engine: Any) -> None:
        self._engine = engine

    def generate(self, requests: list[dict[str, Any]], identity: str) -> list[dict[str, Any]]:
        if any(request["release_after_tokens"] for request in requests):
            return self._engine.loop.run_until_complete(self._staggered(requests, identity))

        return self._engine.generate(
            input_ids=[request["input_tokens"] for request in requests],
            sampling_params=[
                {"max_new_tokens": request["output_tokens"], "ignore_eos": True, "temperature": 0}
                for request in requests
            ],
            rid=[f"{identity}/{slot}" for slot in range(len(requests))],
        )

    async def _staggered(self, requests: list[dict[str, Any]], identity: str) -> list[dict[str, Any]]:
        groups: dict[int, list[int]] = {}
        for slot, request in enumerate(requests):
            groups.setdefault(request["release_after_tokens"], []).append(slot)

        # Gates follow observed token progress, rather than assumed execution delays.
        gates = {offset: asyncio.Event() for offset in groups}
        gates[0].set()
        responses: dict[str, dict[str, Any]] = {}

        await asyncio.gather(
            *[
                self._generate_group(requests, identity, slots, offset, gates, responses)
                for offset, slots in groups.items()
            ]
        )

        return [responses[f"{identity}/{slot}"] for slot in range(len(requests))]

    async def _generate_group(
        self,
        requests: list[dict[str, Any]],
        identity: str,
        slots: list[int],
        offset: int,
        gates: dict[int, asyncio.Event],
        responses: dict[str, dict[str, Any]],
    ) -> None:
        await gates[offset].wait()
        stream = await self._engine.async_generate(
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
