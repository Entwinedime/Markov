"""Compare declared work points with actual native batches and warmup coverage."""

from collections import Counter, defaultdict
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
        self._ranks = range(len(prepared["environment"]["devices"]))
        self._features: Counter[tuple[Any, ...]] = Counter()
        self._completed_work: set[tuple[Any, ...]] = set()
        self._finished: dict[int, set[str]] = defaultdict(set)
        self._kernel_operand_fields: dict[tuple[Any, ...], set[str]] = defaultdict(set)
        self._filter_operand_fields: dict[tuple[Any, ...], set[str]] = defaultdict(set)
        self._transfer_executions: dict[tuple[Any, ...], set[str]] = defaultdict(set)

    @staticmethod
    def _member_key(request: dict[str, Any]) -> tuple[int, ...]:
        member = (request["input_tokens"], request["context_tokens"], request["prompt_tokens"])
        if "request_pool_slot" in request:
            member += (request["request_pool_slot"],)
        return member

    @staticmethod
    def _describe_members(members: tuple[tuple[int, ...], ...]) -> list[dict[str, int]]:
        fields = ("input_tokens", "context_tokens", "prompt_tokens", "request_pool_slot")
        return [dict(zip(fields[: len(member)], member, strict=True)) for member in members]

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

            if step == metadata["phase"]:
                work = self._work_key(metadata["phase"], requests, [])
                self._features[(rank, *work[:2])] += 1
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

    def observe_work_facts(
        self,
        rank: int,
        metadata: dict[str, Any],
        *,
        kernel_operand_fields: Iterable[str] = (),
        transfer_executions: Iterable[str] = (),
    ) -> None:
        if self.stage(metadata) != "measurement":
            return

        if "batch_filter" in metadata:
            self._filter_operand_fields[self._filter_key(rank, metadata)].update(kernel_operand_fields)
            return

        # These facts retain native member order. Unlike requested-point
        # coverage, they must not turn one observed permutation into another.
        members = tuple(self._member_key(request) for request in metadata["requests"])
        key = (rank, metadata["phase"], members)
        if kernel_operand_fields:
            self._kernel_operand_fields[key].update(kernel_operand_fields)
        if transfer_executions:
            self._transfer_executions[key].update(transfer_executions)

    @classmethod
    def _describe_facts(cls, facts: dict[tuple[Any, ...], set[str]], field: str) -> list[dict[str, Any]]:
        return [
            {
                "rank": rank,
                "phase": phase,
                "requests": cls._describe_members(members),
                field: sorted(values),
            }
            for (rank, phase, members), values in facts.items()
        ]

    def feature_coverage(
        self,
        *,
        kernel_operand_fields: bool = False,
        transfer_executions: bool = False,
    ) -> dict[str, Any]:
        points = []
        for index, domain in enumerate(self._requested):
            if "batch_filter" in domain:
                selection = domain["batch_filter"]
                observed = sorted(
                    {
                        rank
                        for (rank, members, count, indices), fields in self._filter_operand_fields.items()
                        if count == selection["input_count"]
                        and indices == tuple(selection["retained_indices"])
                        and all(prompt == domain["prompt_tokens"] and generated == 1 for prompt, generated in members)
                        and "batch_filter" in fields
                    }
                )
            else:
                # The declared recipe makes the first K members finish. Actual
                # publication resolves identities rather than assuming order.
                requests = domain["requests"]
                if "pool_slots" in domain:
                    requests = [
                        {**request, "request_pool_slot": slot}
                        for request, slot in zip(requests, domain["pool_slots"], strict=True)
                    ]
                finished = requests[: domain["finished_request_count"]]
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
            missing = [rank for rank in self._ranks if rank not in observed]
            points.append({"index": index, "work": domain, "observed_ranks": observed, "missing_ranks": missing})

        features = []
        for (rank, phase, members), count in sorted(self._features.items()):
            features.append(
                {
                    "rank": rank,
                    "phase": phase,
                    "batch_request_count": len(members),
                    "total_input_tokens": sum(member[0] for member in members),
                    "requests": self._describe_members(members),
                    "observations": count,
                }
            )

        completed = []
        for rank, phase, members, finished_members in sorted(self._completed_work):
            completed.append(
                {
                    "rank": rank,
                    "phase": phase,
                    "requests": self._describe_members(members),
                    "finished_requests": self._describe_members(finished_members),
                }
            )

        coverage = {
            "requested_points": points,
            "missing_point_indices": [point["index"] for point in points if point["missing_ranks"]],
            "observed_batch_features": features,
            "observed_completed_batch_features": completed,
            "boundary": "actual batch work; not proof of node-cost applicability or unobserved feature interpolation",
        }
        if kernel_operand_fields:
            coverage["observed_kernel_operand_fields"] = self._describe_facts(self._kernel_operand_fields, "fields")
            # A filter scope is not another model batch. Qualify its operand
            # coverage only when a physical kernel supplied that actual work.
            coverage["observed_batch_filter_work"] = [
                {
                    "rank": rank,
                    "requests": [
                        {"prompt_tokens": prompt, "generated_tokens": generated} for prompt, generated in members
                    ],
                    "batch_filter": {"input_count": count, "retained_indices": list(indices)},
                    "fields": sorted(fields),
                }
                for (rank, members, count, indices), fields in self._filter_operand_fields.items()
                if fields
            ]
        if transfer_executions:
            coverage["observed_transfer_executions"] = self._describe_facts(self._transfer_executions, "executions")
        return coverage

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
            for rank in self._ranks
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
