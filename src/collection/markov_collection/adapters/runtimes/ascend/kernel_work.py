"""Physical Ascend descriptors and explicitly declared traversal assumptions."""

from __future__ import annotations

from typing import Any


class AscendKernelWork:
    # These implementation families traverse tensor extents. This is a cost
    # assumption, not a claim that values, caches or profiling have no effect.
    _tensor_traversals = {
        "Cast": "_CastAiCore_Cast",
        "Add": "_AddAiCore_Add",
        "Sub": "_SubAiCore_Sub",
        "ClipByValueV2": "_ClipByValueV2AiCore_ClipByValueV2",
        "ReduceMax": "_ReduceMaxAiCore_ReduceMax",
        "ReduceSum": "_ReduceSumOpAiCore_ReduceSum",
        "ConcatD": "_ConcatD_ConcatD",
    }

    def describe(self, interval: dict[str, Any], metadata: dict[str, Any], step: str) -> dict[str, Any] | None:
        kernel = interval.get("kernel")
        if interval["kind"] != "compute" or kernel is None:
            return None

        work = {
            "name": interval["name"],
            "operation": kernel["Type"],
            "input_shapes": kernel["Input Shapes"],
            "output_shapes": kernel["Output Shapes"],
            "input_dtypes": kernel["Input Data Types"],
            "output_dtypes": kernel["Output Data Types"],
            "input_formats": kernel["Input Formats"],
            "output_formats": kernel["Output Formats"],
            "task_type": interval["arguments"]["Task Type"],
            "launch_blocks": int(kernel["Block Dim" if "Block Dim" in kernel else "Block Num"]),
        }
        implementation = self._tensor_traversals.get(work["operation"])
        work["tensor_traversal"] = implementation is not None and work["name"].endswith(implementation)

        return work

    @staticmethod
    def _shapes(shape_text: str) -> list[tuple[int, ...]]:
        return [
            tuple(int(axis) for axis in tensor.split(",")) if tensor else ()
            for tensor in shape_text.strip('"').split(";")
        ]
