"""Normalize independent serving-port observations without constructing a graph."""

from __future__ import annotations

from collections import Counter, defaultdict
from pathlib import Path
from typing import Any

from markov_collection.artifacts.json_file import read_json, write_json


class ServingPortObservations:
    def __init__(self, capture: Path) -> None:
        self.path = capture
        self.directory = capture.parent
        self.capture = read_json(capture)
        self.gaps: list[dict[str, Any]] = []

    def release(self) -> None:
        """Port-only observations have no disposable vendor exports."""

    def _read_ports(self, clock: dict[str, Any]) -> tuple[list[dict[str, Any]], list[dict[str, Any]], set[int]]:
        expected = set(self.capture["expected_ranks"])
        ranks: set[int] = set()
        identities: set[tuple[int, int, str]] = set()
        ports = []
        arrivals = []
        for reference in self.capture["ports"]:
            record = read_json(self.directory / reference)
            rank = record["rank"]
            if type(rank) is not int or rank not in expected or rank in ranks:
                raise ValueError(f"port observation rank is invalid or repeated: {reference}")
            ranks.add(rank)
            arrivals.extend({**arrival, "rank": rank} for arrival in record["arrivals"])

            if record["clock"] != {"name": clock["name"], "implementation": clock["implementation"]}:
                self.gaps.append(
                    {
                        "kind": "observation",
                        "reason": "port_clock_not_associated",
                        "rank": rank,
                        "clock": record["clock"],
                    }
                )

            for port in record["ports"]:
                kind = port["port"]
                if kind not in {"submitted", "published"}:
                    raise ValueError(f"unknown serving port: {kind}")
                identity = (rank, port["metadata"]["batch_id"], kind)
                if identity in identities:
                    raise ValueError(f"repeated serving port: {identity}")

                identities.add(identity)
                ports.append({**port, "rank": rank, "pid": record["pid"]})

        for rank in sorted(expected - ranks):
            self.gaps.append({"kind": "observation", "reason": "port_rank_missing", "rank": rank})

        return ports, arrivals, ranks

    def _coverage(
        self,
        ports: list[dict[str, Any]],
        arrivals: list[dict[str, Any]],
        ranks: set[int],
        requests: list[dict[str, Any]],
    ) -> dict[str, Any]:
        batches: dict[tuple[int, int], set[str]] = defaultdict(set)
        entries = Counter((arrival["rank"], arrival["request_id"]) for arrival in arrivals)
        publications: dict[tuple[int, str], list[tuple[int, dict[str, Any]]]] = defaultdict(list)
        for port in ports:
            batches[(port["rank"], port["metadata"]["batch_id"])].add(port["port"])
            if port["port"] == "published":
                for request in port["result"]["requests"]:
                    publications[(port["rank"], request["id"])].append((port["at_ns"], request))

        for (rank, batch), observed in batches.items():
            for kind in sorted({"submitted", "published"} - observed):
                self.gaps.append(
                    {
                        "kind": "observation",
                        "reason": "batch_port_missing",
                        "rank": rank,
                        "batch_id": batch,
                        "port": kind,
                    }
                )

        first_tokens = 0
        finished = 0
        # Other native members remain in the facts as context. Completeness is
        # required only for the declared requests, separately on each rank.
        for rank in sorted(ranks):
            for request in requests:
                if entries[(rank, request["id"])] != 1:
                    self.gaps.append(
                        {
                            "kind": "observation",
                            "reason": "request_entry_not_unique",
                            "rank": rank,
                            "request_id": request["id"],
                        }
                    )
                progress = sorted(publications[(rank, request["id"])], key=lambda item: item[0])
                if progress and progress[0][1]["generated_tokens"] == 1:
                    first_tokens += 1
                else:
                    self.gaps.append(
                        {
                            "kind": "observation",
                            "reason": "request_first_generation_missing",
                            "rank": rank,
                            "request_id": request["id"],
                        }
                    )

                if any(
                    item["finished"] and item["generated_tokens"] == request["output_tokens"] for _, item in progress
                ):
                    finished += 1
                else:
                    self.gaps.append(
                        {
                            "kind": "observation",
                            "reason": "request_finished_generation_missing",
                            "rank": rank,
                            "request_id": request["id"],
                            "output_tokens": request["output_tokens"],
                        }
                    )

        return {
            "expected_ranks": self.capture["expected_ranks"],
            "observed_ranks": sorted(ranks),
            "ports": dict(Counter(port["port"] for port in ports)),
            "rank_request_entries": len(arrivals),
            "rank_batches": len(batches),
            "declared_requests": len(requests),
            "rank_request_first_generations": first_tokens,
            "rank_request_completions": finished,
        }

    def extract(self) -> Path:
        if self.capture["role"] != "validation" or self.capture["observation_purpose"] != "ports":
            raise ValueError("serving-port observations require a ports validation capture")

        prepared = read_json(self.directory / self.capture["input"])
        environment = read_json(self.directory / self.capture["environment"])
        if environment["steady_state"]["status"] != "steady":
            self.gaps.append({"kind": "observation", "reason": environment["steady_state"]["status"]})
        timing = environment["request_timing"]
        clock = {
            "name": timing["clock"],
            "implementation": timing["clock_implementation"],
            "origin_ns": timing["origin_ns"],
        }
        ports, arrivals, ranks = self._read_ports(clock)
        coverage = self._coverage(ports, arrivals, ranks, prepared["requests"])

        # These are observed method-return ports, not device completion or
        # performance samples. Missing observations never create inferred ports.
        facts = self.directory / "observation_facts.json"
        write_json(facts, {"ports": ports, "arrivals": arrivals, "clock": clock}, compact=True)
        manifest = self.directory / "validation_manifest.json"
        write_json(
            manifest,
            {
                "role": "validation",
                "framework": self.capture["framework"],
                "candidate_id": self.capture["candidate_id"],
                "observation_purpose": "ports",
                "input": self.capture["input"],
                "environment": self.capture["environment"],
                "native_capture": self.path.name,
                "responses": self.capture["responses"],
                "observations": facts.name,
                "boundaries": self.capture["boundaries"],
                "coverage": coverage,
                "gaps": self.gaps,
                "readiness": "partial" if self.gaps else "observed",
            },
        )

        return manifest
