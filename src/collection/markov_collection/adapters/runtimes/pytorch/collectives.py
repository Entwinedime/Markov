"""Declare shape-preserving native collectives missing a PyTorch fake implementation."""

from __future__ import annotations

from typing import Any


class AllReduceGeometry:
    @staticmethod
    def declare(operations: list[dict[str, Any]], values: list[dict[str, Any]]) -> None:
        for operation in operations:
            if (
                operation["name"] != "c10d.allreduce_.default"
                or operation.get("shape_analysis") == "interface_internal"
            ):
                continue

            # All-reduce changes contents in place; the native output list retains each input's geometry.
            for source_id, output_id in zip(operation["inputs"], operation["outputs"], strict=True):
                source, output = values[source_id], values[output_id]
                output["shape_relation"] = source["shape_relation"]
                output["stride_relation"] = source["stride_relation"]
            if all(values[identity]["shape_relation"] is not None for identity in operation["outputs"]):
                operation["shape_error"] = None
                operation["shape_analysis"] = "native_shape_preserving_collective"
