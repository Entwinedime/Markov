"""Acquire the finite contiguous-materialization branches in their native worker context."""

from __future__ import annotations

import sys
from typing import Any

import torch

from markov_collection.adapters.runtimes.pytorch.observation import TorchMarkers


class AscendMaterializations:
    def __init__(self, operators: Any, step: int, dtype: torch.dtype, axes: tuple[str, str]) -> None:
        self.operators = operators
        self.recording = operators.recording
        self.step = step
        self.dtype = dtype
        self.axes = axes
        self.warmed = False
        self.captured = False

    def points(self) -> tuple[tuple[int, int], ...]:

        # Cover no copy, unit-axis Slice, AsStrided, StridedSlice and the other unit-axis Slice.
        return ((1, 1), (2, self.step), (2, self.step + 1), (2, 2 * self.step), (1, 2 * self.step))

    def warmup(self) -> None:
        if self.warmed:
            return
        for rows, columns in self.points():
            value = torch.ones((rows, columns), dtype=self.dtype, device="npu")
            torch.floor_divide(value[:, :: self.step], self.step)
        torch.npu.synchronize()
        self.warmed = True

    def capture(self) -> None:
        if self.captured:
            return
        for rows, columns in self.points():
            for observed in (False, True):
                with self.operators.values.auxiliary():
                    value = torch.ones((rows, columns), dtype=self.dtype, device="npu")
                identity = len(self.recording.batches)
                self.recording.batches.append(
                    {
                        "id": identity,
                        "kind": "primitive",
                        "phase": "primitive",
                        "members": [],
                        "parameters": {self.axes[0]: rows, self.axes[1]: columns},
                        "observation": "source" if observed else "baseline",
                    }
                )
                if observed:
                    self.operators.begin_batch()
                    with self.recording.scope("RuntimePrototype", batch=identity), self.operators:
                        with self.operators.values.metadata():
                            shapes = self.operators.values.shapes
                            rows_axis = self.operators.values.bounded_parameter(self.axes[0], 1, sys.maxsize)
                            columns_axis = self.operators.values.bounded_parameter(
                                self.axes[1], 1, sys.maxsize
                            )
                            shapes.bind_contiguous(value, (rows_axis, columns_axis))
                        torch.floor_divide(value[:, :: self.step], self.step)
                else:
                    with TorchMarkers.scope(f"markov.baseline/{identity}"):
                        torch.floor_divide(value[:, :: self.step], self.step)
                torch.npu.synchronize()
        self.captured = True
