"""I/O response interpolation shared by offline fitting and target projection."""

from __future__ import annotations

import math
from typing import Any


def interpolate_cost_curve(
    points: list[dict[str, Any]], coordinate: float, coordinate_field: str, value_field: str, *, log_value: bool = True
) -> float:
    """Clamp endpoints on a log coordinate; input anchors are already admitted.

    Positive rates/scales use log values; setup costs use linear values, including zero.
    This evaluates coefficients only, without observing traces or selecting work.
    """

    ordered = sorted(points, key=lambda point: float(point[coordinate_field]))
    if coordinate <= float(ordered[0][coordinate_field]):
        return float(ordered[0][value_field])
    if coordinate >= float(ordered[-1][coordinate_field]):
        return float(ordered[-1][value_field])

    for left, right in zip(ordered, ordered[1:]):
        if coordinate > float(right[coordinate_field]):
            continue

        position = math.log(coordinate / float(left[coordinate_field])) / math.log(
            float(right[coordinate_field]) / float(left[coordinate_field])
        )
        left_value, right_value = float(left[value_field]), float(right[value_field])
        if log_value:
            return math.exp(math.log(left_value) + position * math.log(right_value / left_value))

        return left_value + position * (right_value - left_value)

    raise RuntimeError("cost curve interpolation failed")
