"""Acquire the finite contiguous-materialization branches in their native worker context."""

from __future__ import annotations

import sys
from typing import Any

import torch


class AscendMaterializations:
    def __init__(
        self, operators: Any, step: int, dtype: torch.dtype, axes: tuple[str, str], row_multiples: tuple[int, ...]
    ) -> None:
        self.operators = operators
        self.step = step
        self.dtype = dtype
        self.axes = axes
        self.row_multiples = row_multiples
        self.warmed = False
        self.captured = False

    @staticmethod
    def aligned_rows(rows: Any, multiple: int) -> Any:
        return multiple * ((rows + multiple - 1) // multiple)

    def points(self) -> tuple[tuple[int, int, int], ...]:

        # Cover no copy, unit-axis Slice, AsStrided, StridedSlice and the other unit-axis Slice.
        shapes = ((1, 1), (2, self.step), (2, self.step + 1), (2, 2 * self.step), (1, 2 * self.step))
        points = {}
        for multiple in self.row_multiples:
            for rows, columns in shapes:

                # Keep each concrete shape once per row rule; different rules retain their own symbolic meaning.
                key = (multiple, self.aligned_rows(rows, multiple), columns)
                points.setdefault(key, (multiple, rows, columns))
        return tuple(points.values())

    def warmup(self) -> None:
        if self.warmed:
            return
        for multiple, rows, columns in self.points():
            value = torch.ones((self.aligned_rows(rows, multiple), columns), dtype=self.dtype, device="npu")
            torch.floor_divide(value[:, :: self.step], self.step)
        torch.npu.synchronize()
        self.warmed = True

    def capture(self) -> None:
        if self.captured:
            return
        for multiple, rows, columns in self.points():
            for observed in (False, True):
                with self.operators.values.auxiliary():
                    value = torch.ones((self.aligned_rows(rows, multiple), columns), dtype=self.dtype, device="npu")
                with self.operators.prototype(
                    "RuntimePrototype", observed, **{self.axes[0]: rows, self.axes[1]: columns}
                ):
                    if observed:
                        with self.operators.values.metadata():
                            shapes = self.operators.values.shapes
                            rows_axis = self.operators.values.bounded_parameter(self.axes[0], 1, sys.maxsize)
                            columns_axis = self.operators.values.bounded_parameter(self.axes[1], 1, sys.maxsize)
                            shapes.bind_contiguous(value, (self.aligned_rows(rows_axis, multiple), columns_axis))

                    torch.floor_divide(value[:, :: self.step], self.step)
                torch.npu.synchronize()
        self.captured = True
