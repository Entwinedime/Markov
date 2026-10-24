"""Declare operand layouts and CANN's rank-two contiguous materialization choices."""

from __future__ import annotations

import json
from typing import Any

import sympy
import torch
from torch.utils._sympy.functions import FloorDiv


class AscendLayouts:
    @staticmethod
    def slice_conditions(geometry: tuple[Any, ...]) -> tuple[Any, ...]:
        rows, columns, row_stride, column_stride, offset, storage = (
            sympy.sympify(value.node.expr if isinstance(value, torch.SymInt) else value) for value in geometry
        )
        eq, le, lt = sympy.Eq, sympy.Le, sympy.Lt
        both = sympy.And(lt(1, rows), lt(1, columns))
        rows_fit = le(FloorDiv(offset, row_stride) + rows, FloorDiv(storage, row_stride))
        remainder = sympy.Mod(offset, row_stride)

        # CANN 9.0 CanOptimizeContiguous: remove unit axes, then Slice or StridedSlice.
        # https://gitcode.com/cann/ops-math/blob/v9.0.0/conversion/contiguous/op_host/op_api/contiguous.cpp
        domain = sympy.And(
            le(1, rows),
            le(1, columns),
            lt(0, row_stride),
            lt(0, column_stride),
            le(0, offset),
            sympy.Or(eq(rows, 1), eq(columns, 1), le(column_stride, row_stride)),
        )
        noncontiguous = sympy.Or(
            sympy.And(lt(1, columns), lt(1, column_stride)),
            sympy.And(lt(1, rows), sympy.Or(lt(row_stride, columns), lt(columns, row_stride))),
        )
        sliced = sympy.And(
            noncontiguous,
            sympy.Or(
                sympy.And(eq(rows, 1), le(FloorDiv(offset, column_stride) + columns, FloorDiv(storage, column_stride))),
                sympy.And(eq(columns, 1), rows_fit),
                sympy.And(both, eq(column_stride, 1), rows_fit, le(remainder + columns, row_stride)),
            ),
        )
        strided = sympy.And(
            noncontiguous,
            both,
            lt(1, column_stride),
            le(column_stride * columns, row_stride),
            rows_fit,
            lt(remainder + (columns - 1) * column_stride, row_stride),
        )
        return domain, noncontiguous, sliced, strided

    @classmethod
    def materialization(cls, tensor: Any, relations: Any) -> tuple[list[dict[str, Any]], dict[str, Any]] | None:
        actual = (
            *tensor.shape,
            *tensor.stride(),
            tensor.storage_offset(),
            tensor.untyped_storage().nbytes() // tensor.element_size(),
        )
        observed = cls.slice_conditions(actual)
        if not observed[0]:
            return None

        fake = relations.tensor(tensor)
        symbolic = (
            *fake.shape,
            *fake.stride(),
            fake.storage_offset(),
            fake.untyped_storage().nbytes() // fake.element_size(),
        )
        conditions = cls.slice_conditions(symbolic)
        guards = [
            {"condition": relations.expression(condition), "expected": bool(expected), "basis": "cann_contiguous_2d"}
            for condition, expected in zip(conditions, observed, strict=True)
        ]

        # Storage extent affects Slice validity even when the visible shape and strides agree.
        storage = {
            "provider": "CANN 9.0 CanOptimizeContiguous",
            "size": actual[-1],
            "size_relation": relations.expression(symbolic[-1]),
            "offset_relation": relations.expression(symbolic[-2]),
        }
        return guards, storage

    @staticmethod
    def guards(tensor: dict[str, Any]) -> list[dict[str, Any]]:
        shape, relation = tensor["shape"], tensor["shape_relation"]
        if tensor["device"] != "npu" or relation is None or not shape:
            return []

        actual_expected = 1
        contiguous = True
        strided = False
        for size, stride in reversed(list(zip(shape, tensor["stride"], strict=True))):
            strided |= stride != actual_expected
            contiguous &= size == 1 or stride == actual_expected
            actual_expected *= size
        if not strided:
            return []

        strides = tensor["stride_relation"]
        if strides is None:
            raise ValueError("strided tensor requires a symbolic stride declaration")
        if all(isinstance(axis, int) for axis in relation + strides):
            return []

        expected: Any = 1
        axes = []
        for expression, stride_expression in reversed(list(zip(relation, strides, strict=True))):
            axes.append(["or", ["eq", expression, 1], ["eq", stride_expression, expected]])

            if isinstance(expression, int) and isinstance(expected, int):
                expected *= expression
            elif expected == 1:
                expected = expression
            elif expression != 1:
                expected = ["mul", expression, expected]

        # Unit axes ignore their strides; any empty axis makes the whole tensor contiguous.
        empty = ["or", *(["eq", axis, 0] for axis in relation)]
        is_empty = 0 in shape

        guards = [
            {"condition": ["eq", expression, boundary], "expected": observed == boundary}
            for expression, observed in zip(relation, shape, strict=True)
            if not isinstance(expression, int)
            for boundary in (0, 1)
        ]
        guards.append({"condition": ["or", empty, ["and", *axes]], "expected": is_empty or contiguous})
        return guards

    @staticmethod
    def declare(regions: list[dict[str, Any]], operations: list[dict[str, Any]], values: list[dict[str, Any]]) -> None:
        owners: list[int | None] = []
        device_interfaces = set()
        candidates = []
        for region in regions:
            parent = region["parent"]
            owner = owners[parent] if parent is not None else None
            if owner is None and "interface" in region["metadata"]:
                owner = region["id"]
            owners.append(owner)

        for operation in operations:
            tensors = [values[value] for value in operation["inputs"] + operation["outputs"]]
            if not any(tensor["device"] == "npu" for tensor in tensors):
                continue
            owner = owners[operation["region"]]
            if owner is None:
                candidates.append(operation)
            else:
                device_interfaces.add(owner)
        candidates.extend(regions[owner]["metadata"] for owner in sorted(device_interfaces))

        for candidate in candidates:
            previous = [
                guard for guard in candidate.get("execution_guards", []) if guard.get("basis") != "operand_layout"
            ]
            guards = {}
            for value in candidate["inputs"]:

                # The native selector already declares this operand's complete layout classification.
                if "contiguous_storage" in candidate and value == candidate["inputs"][0]:
                    continue
                for guard in AscendLayouts.guards(values[value]):
                    guard["basis"] = "operand_layout"
                    guards[json.dumps(guard["condition"])] = guard

            # This layout boundary does not claim coverage of other native algorithm branches.
            if previous or guards:
                candidate["execution_guards"] = [*previous, *guards.values()]
            else:
                candidate.pop("execution_guards", None)
