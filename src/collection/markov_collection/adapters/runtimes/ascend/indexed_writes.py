"""Declare CANN 9.0/910B choices for nondeterministic dense one-dimensional int64 writes."""

from __future__ import annotations

from typing import Any


class AscendIndexedWrites:

    # CANN 9.0 IsAiCPUSupport on Ascend910B; int64 is outside its atomic dtype set.
    # https://gitcode.com/cann/ops-nn/blob/0413e303e6cd1986b592c3e039f2579ac0f10b62/index/index_put_v2/op_api/aclnn_index_put_impl.cpp
    index_limit = 100
    storage_limit = 5400000

    @classmethod
    def declare(cls, regions: list[dict[str, Any]], values: list[dict[str, Any]]) -> None:
        geometries: list[dict[int, Any]] = []
        for region in regions:
            metadata = region["metadata"]

            # Port geometry is local to the call; a reused tensor may retain a previous scope's relation.
            parent = region["parent"]
            geometry = dict(geometries[parent]) if parent is not None else {}
            geometry.update((item["value"], item["shape"]) for item in metadata.get("shape_contracts", []))
            geometries.append(geometry)

            if metadata.get("interface") != "tensor_assignment":
                continue
            arguments = metadata["arguments"]
            if len(arguments) != 3 or not all(isinstance(item, dict) and "value" in item for item in arguments):
                continue
            target, indices, payload = (values[item["value"]] for item in arguments)
            shapes = [geometry.get(item["value"], values[item["value"]]["shape_relation"]) for item in arguments]
            if any(
                tensor["device"] != "npu"
                or tensor["dtype"] != "torch.int64"
                or len(tensor["shape"]) != 1
                or tensor["stride"] != [1]
                or tensor["offset"] != 0
                for tensor in (target, indices, payload)
            ) or any(shape is None for shape in shapes) or payload["shape"][0] not in (1, indices["shape"][0]):
                continue

            # Leading unit removal remains a separate binding guard; this predicate selects the native backend.
            guard = {
                "condition": [
                    "or",
                    ["lt", cls.index_limit, shapes[1][0]],
                    ["lt", cls.storage_limit, shapes[0][0]],
                ],
                "expected": indices["shape"][0] > cls.index_limit or target["shape"][0] > cls.storage_limit,
                "basis": "cann_int64_index_put",
            }
            previous = [
                item for item in metadata.get("execution_guards", []) if item.get("basis") != guard["basis"]
            ]
            metadata["execution_guards"] = [*previous, guard]
