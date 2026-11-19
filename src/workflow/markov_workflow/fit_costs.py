"""Fit a bounded service model to independent source samples."""

from bisect import bisect_left
from collections import Counter, defaultdict
import json
from statistics import median

from markov_adapters.artifacts import write_json
from markov_workflow.adapters.sglang_cost_reuse import BatchHostReuse
from markov_workflow.cost_requirements import CostRequirements, cost_identity
from markov_workflow.cost_samples import WorkCatalog


class ServiceCurve:
    def __init__(self, rows):
        groups = defaultdict(list)
        payloads = defaultdict(set)
        for row in rows:
            point = tuple(sorted(row["features"]["axes"].items()))
            groups[point].extend(row["observations_ns"])
            if "copy" in row:
                payloads[point].add(("copy", row["copy"]["bytes"], row["copy"]["direction"]))
            if "collective" in row:
                collective = row["collective"]
                payloads[point].add(("collective", collective["dtype"], collective["algorithm"], collective["elements"]))
        self.ambiguous_payloads = sum(len(values) > 1 for values in payloads.values())
        points = [(dict(point), median(values)) for point, values in groups.items() if len(payloads[point]) <= 1]
        if not points:
            raise ValueError("no usable feature points for service work")
        axes = tuple(points[0][0])
        if any(set(values) != set(axes) for values, _ in points):
            raise ValueError("same service work has inconsistent cost features")

        self.bounds = {axis: [min(values[axis] for values, _ in points), max(values[axis] for values, _ in points)]
                       for axis in axes}
        self.knots = [[0, round(median(value for _, value in points))]]
        self.axis = None
        self.points = len(points)

        # Compare interpolation and a constant on the same interior points, withheld one coordinate at a time.
        best_gain = 0
        for axis in axes:
            observed = defaultdict(list)
            for values, cost in points:
                observed[values[axis]].append(cost)
            knots = [[x, round(median(values))] for x, values in sorted(observed.items())]
            gain = self.gain(knots, observed)
            if gain > best_gain:
                best_gain = gain
                self.axis = axis
                self.knots = knots

    @staticmethod
    def interpolate(left, right, value):
        x1, y1 = left
        x2, y2 = right
        return round(y1 + (y2 - y1) * (value - x1) / (x2 - x1))

    @classmethod
    def gain(cls, knots, observations):
        constant_loss, curve_loss, count = 0, 0, 0
        for index, (x, _) in enumerate(knots[1:-1], 1):
            constant = median(value for coord, values in observations.items() if coord != x for value in values)
            estimate = cls.interpolate(knots[index - 1], knots[index + 1], x)
            for actual in observations[x]:
                scale = max(actual, 1)
                constant_loss += abs(constant - actual) / scale
                curve_loss += abs(estimate - actual) / scale
                count += 1
        return (constant_loss - curve_loss) / count if count else 0

    def document(self):
        return {"bounds": self.bounds, "axis": self.axis, "knots": self.knots, "points": self.points,
                "ambiguous_payload_points": self.ambiguous_payloads}

    @classmethod
    def estimate(cls, curve, axes):
        if any(axis not in axes or not lo <= axes[axis] <= hi for axis, (lo, hi) in curve["bounds"].items()):
            return None
        knots = curve["knots"]
        axis = curve["axis"]
        if axis is None:
            return knots[0][1]
        value = axes[axis]
        position = bisect_left(knots, value, key=lambda pair: pair[0])
        if knots[position][0] == value:
            return knots[position][1]
        return cls.interpolate(knots[position - 1], knots[position], value)


class DomainCosts:
    def __init__(self, sources):
        self.definitions = []
        self.indices = {}
        groups = defaultdict(list)
        for source in sources:
            indices = []
            for definition in source["definitions"]:
                key = cost_identity(definition)
                if key not in self.indices:
                    self.indices[key] = len(self.definitions)
                    self.definitions.append(definition)
                indices.append(self.indices[key])
            for row in source["samples"]:
                groups[indices[row["definition"]], row["interval"], row["features"]["phase"]].append(row)

        self.curves = {key: ServiceCurve(rows).document() for key, rows in sorted(groups.items())}

    def query(self, definition, interval, features):
        phase = features["phase"]
        curve = self.curves.get((self.indices.get(definition), interval, phase))
        if curve is None:
            return "unseen_work", None
        estimate = ServiceCurve.estimate(curve, features["axes"])
        return ("outside_sample_ranges", None) if estimate is None else ("covered", estimate)

    def evaluate(self, source):
        definitions = [cost_identity(value) for value in source["definitions"]]
        counts, errors = defaultdict(Counter), defaultdict(list)
        for row in source["samples"]:
            origin = row["definition"]
            kind = source["definitions"][origin]["work"][row["interval"]]["kind"]
            observations = row["observations_ns"]
            status, estimate = self.query(definitions[origin], row["interval"], row["features"])
            counts[kind][status] += len(observations)
            if estimate is None:
                continue
            actual = median(observations)
            if actual > 0:
                errors[kind].append(abs(estimate - actual) / actual)
        return {
            "counts": counts,
            "median_point_relative_error": {kind: median(values) for kind, values in sorted(errors.items())},
            "unmeasured": source["unmeasured"],
        }

    def document(self, domain):
        return {
            "domain": domain, "definitions": self.definitions,
            "models": [{"definition": definition, "interval": interval, "phase": phase, **model}
                       for (definition, interval, phase), model in self.curves.items()],
        }


class FittedCosts:
    """Use one catalogue for fitting, coverage checks and publishing the selected domains."""

    def __init__(self, sources):
        grouped = defaultdict(list)
        for source in sources:
            grouped[cost_identity(source["domain"])].append(source)
        self.domains = {key: DomainCosts(rows) for key, rows in grouped.items()}
        self.descriptions = {key: rows[0]["domain"] for key, rows in grouped.items()}
        self.empty = DomainCosts(())

    def domain(self, identity):
        return self.domains.get(identity, self.empty)

    def document(self):
        return [costs.document(self.descriptions[key]) for key, costs in self.domains.items()]

    @classmethod
    def qualify(cls, sources, demand):
        fitted = cls(sources.values())
        counts, missing = demand.check(fitted)
        reused = BatchHostReuse.match(demand, missing, sources)
        if reused:
            fitted = cls([*sources.values(), *reused])
            counts, missing = demand.check(fitted)
        return fitted, reused, counts, missing


class FitCosts:
    def __init__(self, arguments):
        self.arguments = arguments

    @staticmethod
    def read(path):
        return json.loads(path.read_text())

    def run(self):
        sources = {path.resolve(strict=True): self.read(path) for path in self.arguments.samples}
        demand = CostRequirements()
        for graph_path, identities_path in self.arguments.demand:
            demand.graph(str(graph_path.resolve()), WorkCatalog(graph_path, identities_path))
        for path in self.arguments.gap:
            demand.gap(str(path.resolve()), self.read(path))

        fitted, reused, counts, missing = FittedCosts.qualify(sources, demand)
        checks = []
        for path in self.arguments.check:
            source = self.read(path)
            costs = fitted.domain(cost_identity(source["domain"]))
            checks.append({"source": str(path.resolve()), **costs.evaluate(source)})

        write_json(self.arguments.output, {
            "domains": fitted.document(),
            "sources": [str(path) for path in sources],
            "source_gaps": [source["unmeasured"] for source in sources.values()],
            "reused": reused,
            "checks": checks,
            "demand": demand.report(counts, missing),
        }, replace=False)
