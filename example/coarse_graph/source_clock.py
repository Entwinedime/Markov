"""Check one shared affine device clock against retained source measurements."""

from fractions import Fraction
from math import ceil, floor


class ClockConstraints:
    def __init__(self, graph, samples):
        first, last = samples
        self.device_id, self.first_counter = first["device"], first["counter"]
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
        self.device = [graph["resources"][event["resource"]]["kind"] == "stream" for event in graph["events"]]
        self.points = [self.point(measured["time_ns"], device)
                       for measured, device in zip(graph["evidence"]["events"], self.device, strict=True)]
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

    def at_counter(self, counter):
        position = Fraction(counter - self.first_counter, self.counts)
        return 1 - position, position, self.origin + position * self.duration

    def constrain(self, kind, index, source, target):
        begin, end = self.points[source], self.points[target]
        if begin is None or end is None:
            self.unknown += 1
            return
        self.measured += 1
        if self.conflict is not None:
            return

        # One positive affine clock preserves order; only cross-clock pairs constrain its calibration.
        if self.device[source] == self.device[target]:
            if begin[2] > end[2]:
                self.region.clear()
                self.conflict = {"kind": kind, "index": index, "source": source, "target": target}
            return

        a, b, limit = begin[0] - end[0], begin[1] - end[1], end[2] - begin[2]
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


class ClockRealization:
    """Choose or verify one affine clock realization for an entire observed case."""

    def __init__(self, clocks, graph):
        if graph is None or any(clock.conflict for clock in clocks.values()):
            raise ValueError("joint clock realization requires the complete measured native causal graph")
        self.clocks = clocks
        self.vertices = {rank: tuple(dict.fromkeys(clock.region)) for rank, clock in clocks.items()}
        self.columns, position = {}, 0
        for rank, vertices in self.vertices.items():
            self.columns[rank] = slice(position, position + len(vertices))
            position += len(vertices)
        self.size = position + 1
        self.constraints, self.equalities = {}, []
        self.measured, self.conditioned = 0, 0
        self.solution = None

        # Convex weights preserve each local causal calibration; the final variable certifies strict rounding cells.
        for rank in clocks:
            row = [Fraction(0)] * self.size
            row[self.columns[rank]] = [Fraction(1)] * len(self.vertices[rank])
            self.equalities.append((row, Fraction(1)))
        row = [Fraction(0)] * self.size
        row[-1] = Fraction(1)
        self.restrict(row, 1)

        # Separate processes on one physical device still share one affine counter/RAW relation.
        devices = {}
        for rank, clock in clocks.items():
            reference = devices.setdefault(clock.device_id, rank)
            if reference == rank:
                continue
            other = clocks[reference]
            for counter in (other.first_counter, other.first_counter + other.counts):
                row, limit = self.difference((rank, clock.at_counter(counter)), (reference, other.at_counter(counter)))
                self.equalities.append((row, limit))

        anchors = [(rank, event) for rank, clock in clocks.items()
                   for event, point in enumerate(clock.points) if point is not None]
        causal = graph.projected(anchors)
        for source, targets in causal.next.items():
            rank, event = anchors[source]
            for target in targets:
                peer, endpoint = anchors[target]
                row, limit = self.difference((rank, clocks[rank].points[event]), (peer, clocks[peer].points[endpoint]))
                self.measured += 1

                # Local polygons already imply most paths; retain only additional cross-clock restrictions.
                if sum(max(row[columns]) for columns in self.columns.values()) > limit:
                    self.restrict(row, limit)

    def coefficients(self, rank, point):
        a, b, constant = point
        row = [Fraction(0)] * self.size
        row[self.columns[rank]] = [a * x + b * y for x, y in self.vertices[rank]]
        return row, constant

    def difference(self, source, target):
        left, begin = self.coefficients(*source)
        right, end = self.coefficients(*target)
        return [a - b for a, b in zip(left, right, strict=True)], end - begin

    def restrict(self, row, limit):
        key = tuple(row)
        self.constraints[key] = min(self.constraints.get(key, limit), limit)

    def condition(self, rank, event, raw_ns):
        point = self.clocks[rank].points[event]
        if point is None:
            raise ValueError("consumed completion has no original measurement")
        self.conditioned += 1
        if not self.clocks[rank].device[event]:
            if raw_ns != point[2]:
                raise ValueError("consumed CPU boundary changes its original RAW measurement")
            return

        # ceil(device_time) == raw_ns means raw_ns - 1 < device_time <= raw_ns, not a fitted tolerance.
        row, constant = self.coefficients(rank, point)
        self.restrict(row, raw_ns - constant)
        lower = [-value for value in row]
        lower[-1] = Fraction(1)
        self.restrict(lower, constant - raw_ns + 1)

    def solve(self):
        from sympy.solvers.simplex import InfeasibleLPError, linprog

        rows, limits = zip(*self.constraints.items(), strict=True)
        equations, values = zip(*self.equalities, strict=True)
        try:

            # SymPy is already present in the CPU control environment; solve measured constraints exactly, without fitting.
            _, solution = linprog([0] * (self.size - 1) + [-1], rows, limits, equations, values)
        except InfeasibleLPError as error:
            raise ValueError("native clocks and consumed completions have no joint affine realization") from error
        weights = [Fraction(value) for value in solution]
        if (weights[-1] <= 0 or any(value < 0 for value in weights)
                or any(sum(a * b for a, b in zip(row, weights, strict=True)) > limit for row, limit in self.constraints.items())
                or any(sum(a * b for a, b in zip(row, weights, strict=True)) != value for row, value in self.equalities)):
            raise ValueError("clock solver did not certify the exact measured constraints and rounding cells")
        self.solution = {
            rank: tuple(sum(weight * vertex[axis] for weight, vertex in zip(weights[self.columns[rank]], vertices, strict=True))
                        for axis in (0, 1))
            for rank, vertices in self.vertices.items()
        }
        return self

    def timestamp(self, rank, event):
        point = self.clocks[rank].points[event]
        if point is None:
            return None
        a, b, constant = point
        x, y = self.solution[rank]
        return ceil(a * x + b * y + constant)

    def report(self):
        return {
            "status": "jointly_realized", "device_clocks": len({clock.device_id for clock in self.clocks.values()}),
            "causal_constraints": self.measured, "consumed_measurements": self.conditioned,
            "clock_maps": [{"rank": rank, "device": self.clocks[rank].device_id,
                            "endpoint_corrections_ns": [str(value) for value in corrections]}
                           for rank, corrections in self.solution.items()],
            "scope": "One feasible affine counter/RAW realization, rounded upward to integer nanoseconds. Original calibration uncertainty remains; this is not a unique measured trajectory.",
        }
