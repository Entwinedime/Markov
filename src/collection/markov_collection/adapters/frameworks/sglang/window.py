"""Keep acquisition phases separate until native request and transfer work has drained."""

from __future__ import annotations

import json
import time
from collections.abc import Iterable
from pathlib import Path
from typing import Any

from markov_collection.artifacts import write_json


class ObservationWindow:
    def __init__(self, directory: Path, description: dict[str, Any]) -> None:
        self.directory = directory
        self.participants = description["native_participants"]
        self.requests = {
            phase: set(description[field])
            for phase, field in (
                ("warmup", "warmup_requests"),
                ("baseline", "baseline_requests"),
                ("source", "source_requests"),
            )
        }

        self.phase: str | None = None
        self.native_idle = True
        self.drained = False
        self.prepared_phase: str | None = None
        self.receipts: list[dict[str, Any]] = []

    def begin(self, identity: str) -> None:
        for phase, requests in self.requests.items():
            if identity in requests:
                self.phase = phase
                self.native_idle = False
                self.drained = False
                if phase == "source":

                    # This hook runs at native request dispatch, after receive and rank broadcast.
                    self.receipts.append({"request": identity, "received_ns": time.perf_counter_ns()})
                return

    def observation(self, requests: Iterable[Any]) -> str | None:
        identities = {request.rid for request in requests}
        for phase in ("source", "baseline"):
            if identities and identities <= self.requests[phase]:
                return phase
        return None

    @staticmethod
    def marker(directory: Path, rank: int) -> Path:
        return directory / "runtime" / f"window-rank-{rank}.json"

    @staticmethod
    def request_drain(directory: Path, phase: str) -> None:
        write_json(directory / "runtime/window-close.json", {"phase": phase})

    def close_requested(self) -> bool:
        path = self.directory / "runtime/window-close.json"
        return path.is_file() and json.loads(path.read_text())["phase"] == self.phase

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
