"""Check one shared affine device clock against retained source measurements."""

from fractions import Fraction
from math import ceil, floor


class ClockConstraints:
    def __init__(self, graph, samples):
        first, last = samples
        self.origin, finish = [(sample["begin_raw_ns"] + sample["end_raw_ns"]) // 2 for sample in samples]
        self.duration = finish - self.origin
        self.counts = last["counter"] - first["counter"]
        if (first["device"] != last["device"] or self.counts <= 0
                or not first["begin_raw_ns"] <= first["end_raw_ns"] < last["begin_raw_ns"] <= last["end_raw_ns"]):
            raise ValueError("device clock requires ordered calibration intervals from one device")
        lower = [sample["begin_raw_ns"] - midpoint for sample, midpoint in zip(samples, (self.origin, finish), strict=True)]
        upper = [sample["end_raw_ns"] - midpoint for sample, midpoint in zip(samples, (self.origin, finish), strict=True)]

        # These endpoint corrections are shared by every stream, never chosen independently for each edge.
        self.region = [(Fraction(x), Fraction(y)) for x, y in (
            (lower[0], lower[1]), (upper[0], lower[1]), (upper[0], upper[1]), (lower[0], upper[1]),
        )]
        self.points = [self.point(measured["time_ns"], graph["resources"][event["resource"]]["kind"] == "stream")
                       for event, measured in zip(graph["events"], graph["evidence"]["events"], strict=True)]
        self.measured = 0
        self.unknown = 0
        self.conflict = None

    def point(self, timestamp, device):
        if timestamp is None:
            return None
        if not device:
            return 0, 0, timestamp

        # Invert the interpreter's integer floor exactly; no rounding tolerance or guessed counter is allowed.
        lower = -(-((timestamp - self.origin) * self.counts) // self.duration)
        upper = -(-((timestamp + 1 - self.origin) * self.counts) // self.duration) - 1
        if lower != upper or not 0 <= lower <= self.counts:
            raise ValueError("device timestamp does not identify one counter inside the calibration window")
        position = Fraction(lower, self.counts)
        return 1 - position, position, self.origin + position * self.duration

    def constrain(self, kind, index, source, target):
        begin, end = self.points[source], self.points[target]
        if begin is None or end is None:
            self.unknown += 1
            return
        self.measured += 1
        if self.conflict is not None:
            return

        a, b, limit = begin[0] - end[0], begin[1] - end[1], end[2] - begin[2]
        if a == b == 0 and limit >= 0:
            return
        distances = [a * x + b * y - limit for x, y in self.region]
        if all(value <= 0 for value in distances):
            return

        # Intersect the calibration rectangle with every causal half-plane using exact rational arithmetic.
        clipped = []
        for offset, point in enumerate(self.region):
            following = (offset + 1) % len(self.region)
            other = self.region[following]
            current_distance, next_distance = distances[offset], distances[following]
            if current_distance <= 0:
                clipped.append(point)
            if current_distance * next_distance < 0:
                fraction = current_distance / (current_distance - next_distance)
                clipped.append(tuple(left + fraction * (right - left) for left, right in zip(point, other, strict=True)))
        self.region = clipped
        if not self.region:
            self.conflict = {"kind": kind, "index": index, "source": source, "target": target}

    def window(self, event):
        point = self.points[event]
        if point is None:
            return None

        # Preserve the envelope of the same feasible calibration, rather than asserting its midpoint as exact.
        a, b, timestamp = point
        values = [a * x + b * y + timestamp for x, y in self.region]
        return floor(min(values)), ceil(max(values))

    def report(self):
        return {
            "status": "inconsistent" if self.conflict else "incomplete" if self.unknown else "compatible",
            "measured_constraints": self.measured, "unknown_constraints": self.unknown,
            "common_calibration_feasible": self.conflict is None, "first_conflict": self.conflict,
        }
