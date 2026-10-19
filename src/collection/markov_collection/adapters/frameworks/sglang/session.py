"""Shared bounded warmup, steady-window observation and native failure handling."""

from __future__ import annotations

import json
import signal
from typing import Any

from markov_collection.adapters.frameworks.sglang.request_cohorts import SGLangRequestCohorts
from markov_collection.adapters.frameworks.sglang.warmup import SGLangWarmup
from markov_collection.adapters.runtimes.ascend.compilation import CompilationObservation
from markov_collection.instrumentation.session import CaptureSession


class SGLangSession(CaptureSession):
    def prepare(self) -> None:
        if self.prepared["role"] == "performance":
            request_ids = set(self.prepared["measurement_ids"]) | set(self.prepared["warmup_ids"])
        else:
            request_ids = {request["id"] for request in self.prepared["requests"]}
        prefix = "markov-warmup/"
        while any(identity.startswith(prefix) for identity in request_ids):
            prefix = "_" + prefix

        self.prepared["warmup_prefix"] = prefix
        super().prepare()

    def _warmup(self, engine: Any) -> dict[str, Any]:
        actual = engine.tokenizer_manager.server_args
        page_size = actual.page_size
        capacity = actual.max_running_requests
        prefix = self.prepared["warmup_prefix"]
        points = SGLangWarmup.points(page_size, capacity)
        self._cohorts = SGLangRequestCohorts(engine)
        for index, point in enumerate(points):
            requests = [
                {
                    "input_tokens": [1] * request["prompt_tokens"],
                    "output_tokens": request["output_tokens"],
                    "release_after_tokens": request["release_after_tokens"],
                }
                for request in point["requests"]
            ]
            self._cohorts.generate(requests, f"{prefix}{index}")
            self._reset(engine)

        # Native tokenizer setup installs an asyncio SIGQUIT handler on first use.
        engine.loop.remove_signal_handler(signal.SIGQUIT)
        signal.signal(signal.SIGQUIT, self._handle_worker_failure)

        return {
            "strategy": "paged_extent_bins",
            "page_size_tokens": page_size,
            "request_capacity": capacity,
            "points": points,
            "logical_kv": "empty",
            "in_flight": False,
        }

    def _steady_windows(self, windows: list[dict[str, int]], warmup: dict[str, Any]) -> dict[str, Any]:
        # A client reset reply can precede another rank's journal publication.
        # Read the native warmup observations when qualifying the completed
        # window, rather than retaining a premature host-side snapshot.
        warmup["batches"] = SGLangWarmup.batches(self.directory / "warmup")
        warmup["workspace"] = [
            {
                "request_capacity": count,
                "states": [
                    json.loads(path.read_text(encoding="utf-8"))
                    for path in sorted((self.directory / "warmup_state" / str(count)).glob("*.json"))
                ],
            }
            for count in sorted({point["request_capacity"] for point in warmup["points"]})
        ]

        result = CompilationObservation.windows(self.directory / "compilation", windows)
        workers = {batch["rank"]: batch["pid"] for batch in warmup["batches"]}
        phases = {
            rank: {batch["phase"] for batch in warmup["batches"] if batch["rank"] == rank}
            for rank in range(self.prepared["configuration"]["tp_size"])
        }
        live_prefills = {
            (batch["rank"], batch["point"])
            for batch in warmup["batches"]
            if batch["phase"] == "prefill"
            and batch["running_requests"]
            and f"{self.prepared['warmup_prefix']}{batch['point']}/1" in batch["requests"]
        }
        missing_live = [
            {"rank": rank, "point": index}
            for rank in phases
            for index, point in enumerate(warmup["points"])
            if point["mechanism"] == "running_prefill" and (rank, index) not in live_prefills
        ]
        missing = [
            rank
            for rank, observed in phases.items()
            if observed != {"prefill", "decode"} or workers[rank] not in result["observed_processes"]
        ]
        result["unconfirmed_ranks"] = missing

        # Cache state is a separate fact from observed compilation coverage.
        state_ranks = set(phases)
        for cohort in warmup["workspace"]:
            state_ranks.intersection_update(
                state["rank"]
                for state in cohort["states"]
                if state["initial"] is not None and state["probe_installation"] is not None
            )
        state_missing = sorted(set(phases) - state_ranks)
        result["unconfirmed_initial_state_ranks"] = state_missing
        result["unconfirmed_live_admissions"] = missing_live

        if missing:
            result["status"] = "observer_not_confirmed"
        elif state_missing:
            result["status"] = "initial_state_not_observed"
        elif missing_live:
            result["status"] = "warmup_live_admission_not_observed"
        return result

    def _reset(self, engine: Any) -> None:
        # Client replies can precede native batch cleanup. The native deferred
        # RPC waits for scheduler idleness; the attempt monitor owns the deadline.
        result = engine.loop.run_until_complete(
            engine.tokenizer_manager.flush_cache(timeout_s=self.prepared["permission"]["max_wall_time_sec"])
        )
        if not result.success:
            raise RuntimeError(f"native request/cache reset failed: {result.message}")
