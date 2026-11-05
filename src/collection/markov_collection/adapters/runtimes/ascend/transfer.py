"""Observe synchronous memfabric copies without treating enqueue as remote completion."""

from __future__ import annotations

import logging
import os
import re
import threading
from contextlib import nullcontext
from pathlib import Path
from typing import Any

from markov_collection.artifacts import write_json
from markov_collection.instrumentation.recording import Recording


class MemfabricObservation:
    service = {
        "kind": "transfer", "resource": "transfer", "identity": 0,
        "name": "memfabric.batch_transfer_sync_write",
    }

    def __init__(self, recording: Recording) -> None:
        self.recording = recording
        self.allocations: list[dict[str, int]] = []
        self.copies: list[dict[str, Any]] = []

    def install(self) -> None:
        import memfabric_hybrid

        memfabric_hybrid.set_extern_logger(self.resource)
        memfabric_hybrid.set_log_level(1)

    def resource(self, level: int, message: str) -> None:
        allocation = re.search(r"init stream ok, stream:(\d+) sq:(\d+) cq:(\d+) logic:(\d+) ssid:(\d+)", message)
        if allocation is not None:
            thread = threading.get_native_id()
            queue = dict(zip(("stream", "sq", "cq", "logic_cq", "ssid"), map(int, allocation.groups()), strict=True))

            # The SDK invokes its external logger synchronously on the stream's allocating thread.
            self.allocations.append({"thread": thread, **queue})
        if level >= 2:
            logging.getLogger(__name__).log(10 * (level + 1), "%s", message)

    def write(self, directory: Path) -> None:
        if self.copies:
            write_json(
                directory / f"{os.getpid()}-memfabric.json", {"allocations": self.allocations, "copies": self.copies}
            )

    @staticmethod
    def declare(regions: list[dict[str, Any]]) -> None:
        for region in regions:
            if region["role"] != "RemoteCopy":
                continue

            metadata = region["metadata"]
            if metadata["status"] != 0:
                raise ValueError("a failed native transfer cannot provide a completed-copy template")

            # The synchronous API owns native segment coalescing and stream completion as one service.
            metadata.update(
                interface="remote_copy",
                entrypoint="remote_copy",
                entrypoint_phase=None,
                arguments=[{"scalar": "transfer_bytes"}, {"scalar": "transfer_segments"}],
                keywords={"mapping": []},
                inputs=[],
                outputs=[],
                writes=[],
                parameters={"transfer_bytes": metadata["bytes"], "transfer_segments": len(metadata["segments"])},
                attributes={"transport": metadata["transport"]},

                # One worker owns one opaque transfer engine; its private queues remain observation evidence.
                native_work=dict(MemfabricObservation.service),
            )

    def copy(self, original: Any, engine: Any, session: str, buffers: Any, destinations: Any, lengths: Any) -> Any:
        region = self.recording.current_scope.get()
        baseline = self.recording.current_baseline.get()
        if region is None and baseline is None:
            return original(engine, session, buffers, destinations, lengths)

        # Lengths describe submitted buffers; the native transport may coalesce adjacent ranges.
        context = nullcontext()
        if region is not None:
            context = self.recording.scope(
                "RemoteCopy",
                transport="ascend_sdma",
                bytes=sum(lengths),
                segments=list(lengths),
                destination_session=session,
                source_addresses=list(buffers),
                destination_addresses=list(destinations),
            )
        with context as scope:
            result = original(engine, session, buffers, destinations, lengths)
            self.copies.append({
                "region": scope.identity if scope is not None else None, "baseline": baseline, "status": result,
            })
            if scope is not None:
                scope.metadata["status"] = result
            return result
