"""Compare declared work points with actual native batches and warmup coverage."""

from collections import defaultdict
from collections.abc import Iterable
from typing import Any


class SamplingDomain:
    def __init__(self, prepared: dict[str, Any]) -> None:
        self.measurement_ids = set(prepared["measurement_ids"])
        self.warmup_ids = set(prepared["warmup_ids"])
        self._warmed: set[tuple[Any, ...]] = set()
        self._missing: dict[tuple[Any, ...], dict[str, Any]] = {}
        self._warmup_records = 0
        self._measurement_records = 0
        self._requested = prepared["measurement_requests"]
        self.ranks = range(len(prepared["environment"]["devices"]))
        self._completed_work: set[tuple[Any, ...]] = set()
        self._finished: dict[int, set[str]] = defaultdict(set)
        self._filter_work: set[tuple[Any, ...]] = set()

    @staticmethod
    def _member_key(request: dict[str, Any]) -> tuple[int, ...]:
        member = (request["input_tokens"], request["context_tokens"], request["prompt_tokens"])
        if "request_pool_slot" in request:
            member += (request["request_pool_slot"],)
        return member

    @classmethod
    def _work_key(
        cls, phase: str, requests: list[dict[str, Any]], finished_requests: list[dict[str, Any]]
    ) -> tuple[Any, ...]:
        # Keep each observed reservation attached to its member's workload.
        # Missing historical slots stay absent, never inferred from shapes.
        members = tuple(sorted(cls._member_key(request) for request in requests))
        finished_members = tuple(sorted(cls._member_key(request) for request in finished_requests))
        return phase, members, finished_members

    def stage(self, metadata: dict[str, Any]) -> str:
        ids = {request["id"] for request in metadata["requests"]}
        if not ids:
            ids = set(metadata["scope_request_ids"])
        if ids <= self.warmup_ids:
            return "warmup"
        if ids <= self.measurement_ids:
            return "measurement"

        raise ValueError("native sampling batch mixes or contains undeclared measurement identities")

    def observe(self, rank: int, step: str, metadata: dict[str, Any]) -> None:
        if step == "filter":
            self._observe_filter(rank, metadata)
            return

        features = ("input_tokens", "context_tokens", "prompt_tokens", "generated_tokens")
        requests = [{name: request[name] for name in features} for request in metadata["requests"]]
        for request, observed in zip(requests, metadata["requests"], strict=True):
            if "request_pool_slot" in observed:
                request["request_pool_slot"] = observed["request_pool_slot"]
        key = (rank, step, metadata["phase"], tuple(tuple(request.values()) for request in requests))
        if step == "publish":
            finished_ids = set(metadata["finished_request_ids"])
            self._finished[rank].update(finished_ids)

        if self.stage(metadata) == "warmup":
            self._warmed.add(key)
            self._warmup_records += 1
        else:
            self._measurement_records += 1
            if key not in self._warmed:
                self._missing[key] = {"rank": rank, "step": step, "phase": metadata["phase"], "requests": requests}

            if step == "schedule" and metadata["phase"] == "idle":
                # Selected completes an empty decision; it has no Publish.
                work = self._work_key(metadata["phase"], requests, [])
                self._completed_work.add((rank, *work))
            if step == "publish":
                # Completion belongs to the native request identity. Native
                # batch order need not retain the sampling recipe's member order.
                finished = [request for request in metadata["requests"] if request["id"] in finished_ids]
                work = self._work_key(metadata["phase"], requests, finished)
                self._completed_work.add((rank, *work))

    @staticmethod
    def _filter_key(rank: int, metadata: dict[str, Any]) -> tuple[Any, ...]:
        selection = metadata["batch_filter"]
        members = tuple((request["prompt_tokens"], request["generated_tokens"]) for request in metadata["requests"])
        return rank, members, selection["input_count"], tuple(selection["retained_indices"])

    def _observe_filter(self, rank: int, metadata: dict[str, Any]) -> None:
        key = ("filter", *self._filter_key(rank, metadata))
        if self.stage(metadata) == "warmup":
            self._warmed.add(key)
            self._warmup_records += 1
        else:
            self._measurement_records += 1
            if key not in self._warmed:
                self._missing[key] = {
                    "rank": rank,
                    "step": "filter",
                    "requests": metadata["requests"],
                    "batch_filter": metadata["batch_filter"],
                }

    def observe_filter_work(self, rank: int, metadata: dict[str, Any], operand_fields: Iterable[str]) -> None:
        # A filter point needs a physical kernel with the actual typed selection;
        # observing a same-sized model batch cannot establish this coverage.
        if "batch_filter" not in metadata or "batch_filter" not in operand_fields:
            return

        if self.stage(metadata) == "measurement":
            self._filter_work.add(self._filter_key(rank, metadata))

    def feature_coverage(self) -> dict[str, Any]:
        points = []
        for index, domain in enumerate(self._requested):
            if "batch_filter" in domain:
                selection = domain["batch_filter"]
                observed = sorted(
                    {
                        rank
                        for rank, members, count, indices in self._filter_work
                        if count == selection["input_count"]
                        and indices == tuple(selection["retained_indices"])
                        and all(prompt == domain["prompt_tokens"] and generated == 1 for prompt, generated in members)
                    }
                )
            else:
                # Completion is attached to each member; a partial finish never
                # reorders its workload or native reservation coordinates.
                requests = domain["requests"]
                if "pool_slots" in domain:
                    requests = [
                        {**request, "request_pool_slot": slot}
                        for request, slot in zip(requests, domain["pool_slots"], strict=True)
                    ]
                finished = [request for request in requests if request["finishes"]]
                work = self._work_key(domain["phase"], requests, finished)
                observed = set()
                for rank, phase, members, finished_members in self._completed_work:
                    # Unconstrained request domains still match either kind of
                    # observation; slot-constrained domains require exact facts.
                    if "pool_slots" not in domain:
                        members = tuple(member[:3] for member in members)
                        finished_members = tuple(member[:3] for member in finished_members)
                    if (phase, members, finished_members) == work:
                        observed.add(rank)
                observed = sorted(observed)
            missing = [rank for rank in self.ranks if rank not in observed]
            points.append({"index": index, "work": domain, "observed_ranks": observed, "missing_ranks": missing})

        return {
            "requested_points": points,
            "missing_point_indices": [point["index"] for point in points if point["missing_ranks"]],
            "boundary": "actual batch work; not proof of node-cost applicability or unobserved feature interpolation",
        }

    def coverage(self) -> dict[str, Any]:
        return {
            "warmup_records": self._warmup_records,
            "measurement_records": self._measurement_records,
            "missing_conditions": list(self._missing.values()),
        }

    def completion_coverage(self) -> dict[str, Any]:
        declared = self.measurement_ids | self.warmup_ids
        ranks = [
            {
                "rank": rank,
                "observed_finished_requests": len(declared & self._finished[rank]),
                "missing_request_ids": sorted(declared - self._finished[rank]),
            }
            for rank in self.ranks
        ]
        missing = {identity for rank in ranks for identity in rank["missing_request_ids"]}
        # A request is completely observed only when every declared rank records
        # its final publication; a union would conceal a missing worker journal.
        return {
            "declared_requests": len(declared),
            "observed_finished_requests": len(declared - missing),
            "missing_request_ids": sorted(missing),
            "ranks": ranks,
        }
