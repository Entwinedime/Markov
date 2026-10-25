"""Observe synchronous memfabric copies without treating enqueue as remote completion."""

from __future__ import annotations

from typing import Any

from markov_collection.instrumentation.recording import Recording


class MemfabricObservation:
    def __init__(self, recording: Recording) -> None:
        self.recording = recording

    @staticmethod
    def declare(regions: list[dict[str, Any]]) -> None:
        for region in regions:
            if region["role"] != "RemoteCopy":
                continue

            metadata = region["metadata"]
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
                native_work={"kind": "transfer", "resource": "transfer", "name": "memfabric.batch_transfer_sync_write"},
            )

    def copy(self, original: Any, engine: Any, session: str, buffers: Any, destinations: Any, lengths: Any) -> Any:
        if self.recording.current_scope.get() is None:
            return original(engine, session, buffers, destinations, lengths)

        # Lengths come from the registered buffers and actual contiguous transfer segments.
        with self.recording.scope(
            "RemoteCopy", transport="ascend_sdma", bytes=sum(lengths), segments=list(lengths)
        ) as scope:
            result = original(engine, session, buffers, destinations, lengths)
            scope.metadata["status"] = result
            return result
