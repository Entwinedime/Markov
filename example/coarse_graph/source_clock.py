"""Check shared device clocks using only independently observed calibration knots."""

from bisect import bisect_right
from collections import defaultdict
from dataclasses import dataclass
from fractions import Fraction
from itertools import combinations
from math import ceil, floor


@dataclass(frozen=True, slots=True)
class CounterPoint:
    segment: int
    position: Fraction
    nominal_ns: Fraction


class ClockSegment:
    def __init__(self, first, last):
        self.first_counter = first["counter"]
        self.counts = last["counter"] - first["counter"]
        self.origin, finish = [(sample["begin_raw_ns"] + sample["end_raw_ns"]) // 2 for sample in (first, last)]
        self.duration = finish - self.origin
        lower = [sample["begin_raw_ns"] - midpoint for sample, midpoint in zip((first, last), (self.origin, finish), strict=True)]
        upper = [sample["end_raw_ns"] - midpoint for sample, midpoint in zip((first, last), (self.origin, finish), strict=True)]
        self.region = [(Fraction(x), Fraction(y)) for x, y in (
            (lower[0], lower[1]), (upper[0], lower[1]), (upper[0], upper[1]), (lower[0], upper[1]),
        )]
        self.causes = [None] * len(self.region)
        self.conflict = None

    def point(self, timestamp, segment):

        # Invert the producer's integer floor exactly; a rounded timestamp must identify one actual counter.
        lower = -(-((timestamp - self.origin) * self.counts) // self.duration)
        upper = -(-((timestamp + 1 - self.origin) * self.counts) // self.duration) - 1
        if lower != upper or not 0 <= lower <= self.counts:
            raise ValueError("device timestamp does not identify one counter inside its sampled clock segment")
        return self.at_counter(self.first_counter + lower, segment)

    def at_counter(self, counter, segment):
        position = Fraction(counter - self.first_counter, self.counts)
        return CounterPoint(segment, position, self.origin + position * self.duration)

    def clip(self, a, b, limit, cause):
        distances = [a * x + b * y - limit for x, y in self.region]
        if all(value <= 0 for value in distances):
            return

        # Each vertex retains the cause of its incoming edge, rather than a ledger of all observed relations.
        clipped, causes = [], []
        for offset, point in enumerate(self.region):
            following = (offset + 1) % len(self.region)
            other = self.region[following]
            current_distance, next_distance = distances[offset], distances[following]
            if current_distance <= 0:
                clipped.append(point)
                causes.append(cause if current_distance == 0 and distances[offset - 1] > 0 else self.causes[offset])
            if current_distance * next_distance < 0:
                fraction = current_distance / (current_distance - next_distance)
                clipped.append(tuple(left + fraction * (right - left) for left, right in zip(point, other, strict=True)))
                causes.append(self.causes[following] if current_distance < 0 else cause)
        if not clipped:
            self.conflict = {"witness": [value for value in self.causes if value is not None],
                             "minimum_violation_ns": str(min(distances))}
            if cause is not None:
                self.conflict["witness"].append(cause)
        self.region, self.causes = clipped, causes

    def projection(self, axis):
        return min(point[axis] for point in self.region), max(point[axis] for point in self.region)

    def intersect_axis(self, axis, interval):
        self.clip(1 - axis, axis, interval[1], None)
        if not self.conflict:
            self.clip(axis - 1, -axis, -interval[0], None)


class ClockConstraints:
    def __init__(self, graph, samples):
        if len(samples) < 2:
            raise ValueError("device clock requires at least two independent samples")
        self.device_id = samples[0]["device"]
        if (any(sample["device"] != self.device_id or sample["begin_raw_ns"] > sample["end_raw_ns"] for sample in samples)
                or any(left["counter"] >= right["counter"] or left["end_raw_ns"] >= right["begin_raw_ns"]
                       for left, right in zip(samples, samples[1:]))):
            raise ValueError("device clock requires advancing, separated calibration intervals from one device")
        self.samples = samples
        self.counters = [sample["counter"] for sample in samples]
        self.midpoints = [(sample["begin_raw_ns"] + sample["end_raw_ns"]) // 2 for sample in samples]
        self.segments = [ClockSegment(first, last) for first, last in zip(samples, samples[1:])]
        self.device = [graph["resources"][event["resource"]]["kind"] == "stream" for event in graph["events"]]
        self.points = [self.point(measured["time_ns"], device)
                       for measured, device in zip(graph["evidence"]["events"], self.device, strict=True)]
        self.measured, self.unknown = 0, 0
        self.conflict = None

    def point(self, timestamp, device):
        if timestamp is None or not device:
            return timestamp
        index = min(max(bisect_right(self.midpoints, timestamp) - 1, 0), len(self.segments) - 1)
        return self.segments[index].point(timestamp, index)

    def at_counter(self, counter):
        if not self.counters[0] <= counter <= self.counters[-1]:
            raise ValueError("clock realization cannot extrapolate outside observed device counters")
        index = min(bisect_right(self.counters, counter) - 1, len(self.segments) - 1)
        return self.segments[index].at_counter(counter, index)

    @staticmethod
    def form(point):
        if isinstance(point, CounterPoint):
            return 1 - point.position, point.position, point.nominal_ns
        return 0, 0, point

    def constrain(self, kind, index, source, target):
        begin, end = self.points[source], self.points[target]
        if begin is None or end is None:
            self.unknown += 1
            return
        self.measured += 1
        if self.conflict is not None:
            return
        cause = {"kind": kind, "index": index, "source": source, "target": target}
        left, right = self.form(begin), self.form(end)

        # Ordered snapshot intervals make every segment positive; counter order needs no edge-specific fit.
        if self.device[source] == self.device[target]:
            if left[2] > right[2]:
                self.conflict = {**cause, "witness": [cause]}
            return
        point = begin if self.device[source] else end
        segment = self.segments[point.segment]
        segment.clip(left[0] - right[0], left[1] - right[1], right[2] - left[2], cause)
        if segment.conflict:
            self.conflict = {**cause, "segment": point.segment, **segment.conflict}

    def finish(self):
        if self.conflict:
            return self

        # Convex binary relations form a path: two projection passes give exact shared-knot consistency.
        pairs = [(index, 0, index - 1, 1) for index in range(1, len(self.segments))]
        pairs += [(index, 1, index + 1, 0) for index in range(len(self.segments) - 2, -1, -1)]
        for index, axis, adjacent, peer_axis in pairs:
            interval = self.segments[adjacent].projection(peer_axis)
            segment = self.segments[index]
            segment.intersect_axis(axis, interval)
            if segment.conflict:
                self.conflict = {"kind": "shared_snapshot", "segment": index, "sample": index + axis,
                                 "required_correction_ns": [str(value) for value in interval],
                                 "minimum_violation_ns": segment.conflict["minimum_violation_ns"],
                                 "related_constraints": segment.conflict["witness"] + [
                                     value for value in self.segments[adjacent].causes if value is not None]}
                break
        return self

    def explain(self, graph):
        constraints = []
        for cause in self.conflict.get("witness", self.conflict.get("related_constraints", [])):
            context = {"kind": cause["kind"], "index": cause["index"]}
            for side in ("source", "target"):
                event = cause[side]
                owner = graph["events"][event]
                scope = graph["scopes"][owner["scope"]]
                context[side] = {
                    "event": event, "scope": owner["scope"], "role": scope["role"], "site": scope["site"],
                    "resource": graph["resources"][owner["resource"]], "measurement": graph["evidence"]["events"][event],
                }
                point = self.points[event]
                if isinstance(point, CounterPoint):
                    segment = self.segments[point.segment]
                    context[side]["device_counter"] = str(segment.first_counter + point.position * segment.counts)
            constraints.append(context)
        index = self.conflict.get("segment")
        return {"first_conflict": self.conflict, "calibration": self.samples if index is None else self.samples[index:index + 2],
                "constraints": constraints}

    def window(self, event):
        point = self.points[event]
        if point is None or not isinstance(point, CounterPoint):
            return None if point is None else (point, point)
        a, b, timestamp = self.form(point)
        values = [a * x + b * y + timestamp for x, y in self.segments[point.segment].region]
        return floor(min(values)), ceil(max(values))

    def report(self):
        return {
            "status": "inconsistent" if self.conflict else "incomplete" if self.unknown else "compatible",
            "calibration_samples": len(self.samples), "measured_constraints": self.measured,
            "unknown_constraints": self.unknown, "common_calibration_feasible": self.conflict is None,
            "first_conflict": self.conflict,
        }


class ClockRealization:
    """Certify one shared clock whose only knots are actual independent snapshots."""

    def __init__(self, clocks, graph):
        if graph is None or any(clock.conflict for clock in clocks.values()):
            raise ValueError("joint clock realization requires the complete measured native causal graph")
        self.clocks = clocks
        self.columns, self.origins, self.vertices = {}, {}, {}
        self.size = 0
        for rank, clock in clocks.items():
            self.columns[rank] = self.size
            self.size += len(clock.samples)
            self.origins[rank] = [sample["begin_raw_ns"] - midpoint
                                  for sample, midpoint in zip(clock.samples, clock.midpoints, strict=True)]
            for index, segment in enumerate(clock.segments):
                self.vertices[rank, index] = tuple(dict.fromkeys(segment.region))
        self.size += 1
        self.constraints, self.equalities = {}, []
        self.samples = defaultdict(list)
        self.measured, self.conditioned = 0, 0
        self.solution = None

        # Adjacent polygons use the same variable for their shared independently measured snapshot.
        for (rank, index), vertices in self.vertices.items():
            ox, oy = self.origins[rank][index:index + 2]
            points = [(x - ox, y - oy) for x, y in vertices]
            column = self.columns[rank] + index
            for axis in (0, 1):
                self.restrict({column + axis: 1}, max(point[axis] for point in points))
            for (x, y), (other_x, other_y) in zip(points, points[1:] + points[:1], strict=True):
                dx, dy = other_x - x, other_y - y
                self.restrict({column: dy, column + 1: -dx}, dy * x - dx * y)
        self.restrict({self.size - 1: 1}, 1)

        # Overlapping records share one physical function at the union of their observed breakpoints.
        for (rank, clock), (peer, other) in combinations(clocks.items(), 2):
            if clock.device_id != other.device_id:
                continue
            begin, end = max(clock.counters[0], other.counters[0]), min(clock.counters[-1], other.counters[-1])
            if begin <= end:
                counters = {begin, end} | {value for owner in (clock, other) for value in owner.counters if begin < value < end}
                for counter in sorted(counters):
                    row, limit, _ = self.difference((rank, clock.at_counter(counter)), (peer, other.at_counter(counter)))
                    self.equalities.append((row, limit))
            else:
                before, after = ((rank, clock), (peer, other)) if clock.counters[-1] < other.counters[0] else ((peer, other), (rank, clock))
                row, limit, _ = self.difference((before[0], before[1].at_counter(before[1].counters[-1])),
                                                (after[0], after[1].at_counter(after[1].counters[0])))
                self.restrict(row, limit)

        anchors = [(rank, event) for rank, clock in clocks.items()
                   for event, point in enumerate(clock.points) if point is not None]
        causal = graph.projected(anchors)
        for source, targets in causal.next.items():
            rank, event = anchors[source]
            for target in targets:
                peer, endpoint = anchors[target]
                row, limit, support = self.difference((rank, clocks[rank].points[event]), (peer, clocks[peer].points[endpoint]))
                self.measured += 1
                if support > limit:
                    self.restrict(row, limit)

    def coefficients(self, rank, point):
        if not isinstance(point, CounterPoint):
            return {}, point, None
        a, b, constant = self.clocks[rank].form(point)
        column = self.columns[rank] + point.segment
        x, y = self.origins[rank][point.segment:point.segment + 2]
        return {column: a, column + 1: b}, constant + a * x + b * y, (rank, point.segment, a, b)

    def difference(self, source, target):
        left, begin, first = self.coefficients(*source)
        right, end, last = self.coefficients(*target)
        row = dict(left)
        for column, value in right.items():
            row[column] = row.get(column, 0) - value

        # Support is an upper bound; combine same-segment terms before bounding shared-knot terms.
        groups = defaultdict(lambda: [0, 0])
        for term, sign in ((first, 1), (last, -1)):
            if term is not None:
                rank, index, a, b = term
                groups[rank, index][0] += sign * a
                groups[rank, index][1] += sign * b
        support = 0
        for (rank, index), (a, b) in groups.items():
            ox, oy = self.origins[rank][index:index + 2]
            support += max(a * (x - ox) + b * (y - oy) for x, y in self.vertices[rank, index])
        return row, end - begin, support

    def restrict(self, row, limit):
        key = tuple(sorted((column, value) for column, value in row.items() if value))
        if not key and limit < 0:
            raise ValueError("native causal relation contradicts fixed RAW measurements")
        if key:
            self.constraints[key] = min(self.constraints.get(key, limit), limit)

    def condition(self, rank, event, raw_ns):
        point = self.clocks[rank].points[event]
        if point is None:
            raise ValueError("consumed completion has no original measurement")
        self.conditioned += 1
        if not isinstance(point, CounterPoint):
            if raw_ns != point:
                raise ValueError("consumed CPU boundary changes its original RAW measurement")
            return
        self.samples[rank, point.segment].append((event, raw_ns))

    def envelope(self, rank, index, lower):
        ordinates = {}
        for event, raw_ns in self.samples[rank, index]:
            position = self.clocks[rank].points[event].position
            if position in ordinates:
                raw_ns = min(ordinates[position], raw_ns) if lower else max(ordinates[position], raw_ns)
            ordinates[position] = raw_ns
        chain = []
        for point in sorted(ordinates.items()):
            while len(chain) >= 2:
                left, middle = chain[-2:]
                turn = ((middle[0] - left[0]) * (point[1] - middle[1])
                        - (middle[1] - left[1]) * (point[0] - middle[0]))
                convex = turn > 0 if lower else turn < 0
                if convex:
                    break
                chain.pop()
            chain.append(point)
        return chain

    def solve(self):
        from scipy.optimize import linprog
        from scipy.sparse import coo_matrix
        from sympy import Rational, SparseMatrix, linsolve, symbols

        # Each segment obeys all strict ceil cells exactly when it obeys their lower and upper envelopes.
        for rank, index in self.vertices:
            clock = self.clocks[rank]
            for lower in (True, False):
                for position, raw_ns in self.envelope(rank, index, lower):
                    owner = clock.segments[index]
                    point = CounterPoint(index, position, owner.origin + position * owner.duration)
                    row, constant, _ = self.coefficients(rank, point)
                    if lower:
                        self.restrict(row, raw_ns - constant)
                    else:
                        row = {column: -value for column, value in row.items()}
                        row[self.size - 1] = 1
                        self.restrict(row, constant - raw_ns + 1)
        inequalities = [(dict(row), limit) for row, limit in self.constraints.items()]

        def matrix(rows):
            entries = [(index, column, float(value)) for index, (row, _) in enumerate(rows) for column, value in row.items() if value]
            return coo_matrix(([value for _, _, value in entries],
                               ([index for index, _, _ in entries], [column for _, column, _ in entries])),
                              shape=(len(rows), self.size)).tocsr()

        # HiGHS proposes a sparse active basis; numerical tolerances never determine acceptance of evidence.
        result = linprog([0] * (self.size - 1) + [-1], matrix(inequalities), [float(limit) for _, limit in inequalities],
                         matrix(self.equalities) if self.equalities else None,
                         [float(value) for _, value in self.equalities] if self.equalities else None, method="highs")
        if not result.success:
            raise ValueError(f"clock candidate unavailable; exact joint realization remains unverified: {result.message}")
        active = self.equalities + [row for row, residual in zip(inequalities, result.ineqlin.residual, strict=True) if residual == 0]
        active += [({column: 1}, 0) for column, residual in enumerate(result.lower.residual) if residual == 0]
        equations = SparseMatrix(len(active), self.size, {
            (index, column): Rational(value.numerator, value.denominator)
            for index, (row, _) in enumerate(active) for column, value in row.items() if value
        })
        constants = SparseMatrix([Rational(value.numerator, value.denominator) for _, value in active])
        variables = symbols(f"d0:{self.size}")
        solutions = list(linsolve((equations, constants), variables))
        if not solutions:
            raise ValueError("numerical clock basis has no exact rational solution")
        proposal = {variable: Rational(float(value)) for variable, value in zip(variables, result.x, strict=True)}
        offsets = [Fraction(value.xreplace(proposal)) for value in solutions[0]]

        def evaluate(row):
            return sum(value * offsets[column] for column, value in row.items())

        if (offsets[-1] <= 0 or any(value < 0 for value in offsets)
                or any(evaluate(row) > limit for row, limit in inequalities)
                or any(evaluate(row) != value for row, value in self.equalities)):
            raise ValueError("clock candidate did not certify the exact measured constraints and strict rounding cells")
        self.solution = {rank: [origin + offsets[self.columns[rank] + index] for index, origin in enumerate(origins)]
                         for rank, origins in self.origins.items()}

        # Certify every original consumption, including duplicates, after compression and basis reconstruction.
        for (rank, _), samples in self.samples.items():
            if any(self.timestamp(rank, event) != raw_ns for event, raw_ns in samples):
                raise ValueError("clock solver changes an original consumed completion")
        return self

    def timestamp(self, rank, event):
        point = self.clocks[rank].points[event]
        if point is None or not isinstance(point, CounterPoint):
            return point
        a, b, constant = self.clocks[rank].form(point)
        x, y = self.solution[rank][point.segment:point.segment + 2]
        return ceil(a * x + b * y + constant)

    def report(self):
        return {
            "status": "jointly_realized", "device_clocks": len({clock.device_id for clock in self.clocks.values()}),
            "causal_constraints": self.measured, "consumed_measurements": self.conditioned,
            "clock_maps": [{"rank": rank, "device": self.clocks[rank].device_id,
                            "snapshot_corrections_ns": [str(value) for value in corrections]}
                           for rank, corrections in self.solution.items()],
            "scope": "One feasible shared piecewise affine counter/RAW realization, using only independently observed snapshot knots and rounded upward to integer nanoseconds. Original calibration uncertainty remains; this is not a unique measured trajectory.",
        }
