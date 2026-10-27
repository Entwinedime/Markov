"""Establish and observe the native workspace bound used by steady serving."""

from __future__ import annotations

import ctypes
from pathlib import Path

import torch_npu

from markov_collection.adapters.runtimes.ascend.synchronization import SynchronizationObservation


class AscendWorkspaces:
    def __init__(self, synchronization: SynchronizationObservation, native_library: Path) -> None:
        self.synchronization = synchronization
        self.native = ctypes.CDLL(str(native_library))

    @staticmethod
    def attention_bound(cube_cores: int, geometries: list[tuple[int, int, int]]) -> int:

        # ATB 9.0 ND FlashAttention uses nine float scratch blocks; split-M doubles the ping-pong block.
        bound = cube_cores * (32768 * 16) * 4 * 9
        for heads, query_dim, value_dim in geometries:
            if query_dim != value_dim or query_dim > 128:
                raise NotImplementedError("workspace warmup covers dense ND attention with equal head dimensions <=128")

            # Native blockDim never exceeds cube cores; replacing split-KV's 0.8 by 1 bounds its two float buffers.
            base = cube_cores * 65536 * (4 * 4 + 2)
            split = cube_cores**2 * heads * (query_dim + 1) * 4
            copies = 4 * cube_cores * 256 * heads * query_dim * 2
            bound = max(bound, base + split + copies)

        # ATB aligns the runner scratch buffer to 512 bytes before handing it to the allocator.
        return ((bound + 511) // 512) * 512

    def reserve(self, cube_cores: int, geometries: list[tuple[int, int, int]], streams: list[int]) -> dict[str, object]:
        required = self.attention_bound(cube_cores, geometries)
        library = ctypes.CDLL(str(Path(torch_npu.__file__).parent / "lib/libtorch_npu.so"))
        allocator = library._ZN6at_npu6native18allocate_workspaceEmPv
        reserve = self.native.markov_reserve_workspace
        reserve.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_void_p]
        reserve.restype = ctypes.c_int

        streams = list(dict.fromkeys(streams))
        torch_npu.npu.synchronize()
        for stream in streams:
            if reserve(ctypes.cast(allocator, ctypes.c_void_p), required, stream) != 0:
                raise RuntimeError("native workspace bound reservation failed")

        minimum = ((required + 32 + 2097151) // 2097152) * 2097152
        retained = {
            state["stream"]: state["capacity_bytes"]
            for state in self.snapshot()
            if state["device"] == torch_npu.npu.current_device()
        }
        established = all(retained.get(self.stream_identity(stream), 0) >= minimum for stream in streams)
        if not established:
            raise RuntimeError("native allocator did not retain the declared workspace capacity")
        return {
            "kind": "native_attention_bound_reservation",
            "reservation_bytes": required,
            "capacity_lower_bound_bytes": minimum,
            "cube_cores": cube_cores,
            "geometries": geometries,
        }

    def snapshot(self) -> list[dict[str, int]]:

        # This boundary is outside observed serving work; pending allocator changes must finish first.
        torch_npu.npu.synchronize()
        memory = torch_npu.npu.memory
        memory._record_memory_history("state", context=None, max_entries=1)
        try:
            snapshot = memory._snapshot()
        finally:
            memory._record_memory_history(None)

        streams = []
        for device, trace in enumerate(snapshot["device_traces"]):
            for entry in trace:
                if entry["action"] != "workspace_snapshot":
                    continue

                streams.append(
                    {"device": device, "stream": self.stream_identity(entry["stream"]), "capacity_bytes": entry["size"]}
                )

        return sorted(streams, key=lambda item: (item["device"], item["stream"]))

    def stream_identity(self, stream: int) -> int:
        identity = ctypes.c_int32(-1)
        status = self.synchronization.get_stream(stream, ctypes.byref(identity))
        if status != 0:
            raise RuntimeError(f"cannot identify retained workspace stream: {status}")
        return identity.value
