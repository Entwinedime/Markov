"""Interpret the cached allocator branch of the pinned torch_npu implementation."""

from dataclasses import dataclass
from typing import Any, ClassVar


@dataclass(frozen=True)
class WorkspaceAllocation:
    source_commit: ClassVar[str] = "94f8a8e6b523d7ba553e1b80d5b5248478391526"

    branch: str
    capacity_bytes: int
    requires_device_synchronization: bool

    @classmethod
    def from_observation(cls, workspace: dict[str, Any]) -> "WorkspaceAllocation":
        # NPUWorkspaceAllocator.cpp: malloc adds 32 bytes and rounds a new
        # cached block to kRoundLarge (2 MiB). These are allocation rules,
        # not a formula for an operation's workspace demand or its latency.
        if workspace["action"] == "release":
            return cls("release", 0, False)

        requested = workspace["requested_bytes"] + 32
        capacity = workspace["capacity_before_bytes"]
        if requested <= capacity:
            return cls("reuse", capacity, False)

        alignment = 2 * 1024 * 1024
        capacity = ((requested + alignment - 1) // alignment) * alignment
        has_allocation = workspace["address_before"] != 0
        return cls("grow" if has_allocation else "allocate", capacity, has_allocation)

    def agrees_with(self, workspace: dict[str, Any]) -> bool:
        if workspace["capacity_after_bytes"] != self.capacity_bytes:
            return False
        if self.branch == "release":
            return workspace["address_after"] == 0
        if self.branch == "reuse":
            return workspace["address_after"] == workspace["address_before"]

        # Growth may reuse the old address. Pointer inequality is not evidence
        # for choosing the branch; only the demand and capacity establish it.
        return workspace["address_after"] != 0

    def describe(self) -> dict[str, Any]:
        return {
            "branch": self.branch,
            "capacity_bytes": self.capacity_bytes,
            "requires_device_synchronization": self.requires_device_synchronization,
        }
