"""Keep acquisition phases separate until native request and transfer work has drained."""

from __future__ import annotations

import json
import time
from collections.abc import Iterable
from pathlib import Path
from typing import Any

from markov_adapters.artifacts import write_json


class ObservationWindow:
    def __init__(self, directory: Path, description: dict[str, Any]) -> None:
        self.directory = directory
        self.participants = description["native_participants"]
        self.requests = {
            phase: set(description[field])
            for phase, field in (
                ("warmup", "warmup_requests"),
                ("source", "source_requests"),
            )
        }
        self.pending = {phase: set(requests) for phase, requests in self.requests.items()}
        aliases = dict(zip((request["id"] for request in description["content"]["requests"]), description["source_requests"], strict=True))
        self.groups = [{aliases[identity] for identity in group} for group in description["submission_groups"][:-1]]
        self.group = 0

        self.phase: str | None = None
        self.drained = False
        self.prepared_phase: str | None = None
        self.receipts: list[dict[str, Any]] = []
        self.receive_rounds: list[dict[str, Any]] = []
        self.ingress: list[dict[str, Any]] = []
        self.input_attempts: list[dict[str, Any]] = []

    def begin(self, identity: str) -> None:
        for phase, requests in self.requests.items():
            if identity in requests:
                self.pending[phase].discard(identity)
                self.phase = phase
                self.drained = False
                if phase == "source":

                    # This hook runs at native request dispatch, after receive and rank broadcast.
                    self.receipts.append({"request": identity, "received_raw_ns": time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)})
                return

    def receive(self, requests: list[str], time_ns: int, *, ingress: bool, attempt=None) -> None:
        observed = [request for request in requests if request in self.requests["source"]]
        if attempt is not None and (observed or (self.phase == "source" and not self.drained)):

            # Keep the first source pull and in-window empty reads, not idle polls before or after the window.
            self.input_attempts.append({**attempt, "requests": observed, "received_raw_ns": time_ns})
        if observed:

            # One receive round may admit several requests; it does not prescribe their execution batches.
            records = self.ingress if ingress else self.receive_rounds
            records.append({"requests": observed, "received_raw_ns": time_ns})

    @staticmethod
    def marker(directory: Path, rank: int) -> Path:
        return directory / "runtime" / f"window-rank-{rank}.json"

    def inputs_complete(self) -> bool:
        return self.phase is not None and not self.pending[self.phase]

    def group_idle(self, rank: int) -> None:
        if self.phase != "source" or self.group == len(self.groups) or self.groups[self.group] & self.pending["source"]:
            return

        # A terminal response can precede native removal from running batches; preserve cache while waiting for idle.
        write_json(self.marker(self.directory, rank), {"phase": "source", "state": f"group/{self.group}"})
        self.group += 1

    def drain(self, rank: int) -> None:
        write_json(self.marker(self.directory, rank), {"phase": self.phase, "state": "drained"})
        self.drained = True

    def complete(self, rank: int, ingress: bool) -> None:
        if ingress and not self.ready(
            self.directory, (peer for peer in self.participants if peer != rank), self.prepared_phase, "complete"
        ):
            return

        # Publish at the native whole-loop idle boundary, after reset and observer preparation.
        write_json(self.marker(self.directory, rank), {"phase": self.prepared_phase, "state": "complete"})
        self.prepared_phase = None
        self.phase = None
        self.drained = False

    @classmethod
    def ready(cls, directory: Path, ranks: Iterable[int], phase: str, state: str) -> bool:
        markers = [cls.marker(directory, rank) for rank in ranks]
        return all(
            path.is_file() and json.loads(path.read_text()) == {"phase": phase, "state": state} for path in markers
        )

    @classmethod
    def wait(cls, directory: Path, ranks: Iterable[int], phase: str, state: str) -> None:
        ranks = tuple(ranks)

        while not cls.ready(directory, ranks, phase, state):
            time.sleep(0.05)
