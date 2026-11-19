"""Fit a bounded service model to independent source samples."""

from bisect import bisect_left
from collections import Counter, defaultdict
import json
from statistics import median

from markov_adapters.artifacts import write_json
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
                key = self.identity(definition)
                if key not in self.indices:
                    self.indices[key] = len(self.definitions)
                    self.definitions.append(definition)
                indices.append(self.indices[key])
            for row in source["samples"]:
                groups[indices[row["definition"]], row["interval"], row["features"]["phase"]].append(row)

        self.curves = {key: ServiceCurve(rows).document() for key, rows in sorted(groups.items())}

    @staticmethod
    def identity(definition):
        return json.dumps(definition, sort_keys=True, separators=(",", ":"))

    def definitions_in(self, definitions):
        return [self.indices.get(self.identity(item)) for item in definitions]

    def query(self, definition, interval, features):
        phase = features["phase"]
        curve = self.curves.get((definition, interval, phase))
        if curve is None:
            return "unseen_work", None
        estimate = ServiceCurve.estimate(curve, features["axes"])
        return ("outside_sample_ranges", None) if estimate is None else ("covered", estimate)

    def evaluate(self, source):
        definitions = self.definitions_in(source["definitions"])
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

    def demand(self, catalog, identities_path):
        definitions = self.definitions_in(catalog.definitions)
        counts, missing = defaultdict(Counter), {}
        for piece, definition, interval, features in catalog:
            status, _ = self.query(definitions[definition], interval, features)
            kind = piece["kind"]
            counts[kind][status] += 1
            if status == "covered":
                continue

            # Group by the exact work requested, retaining coordinate ranges for the next measured recipe.
            phase = features["phase"]
            key = definition, interval, phase, status
            if key not in missing:
                owner = catalog.definitions[definition]
                resource = catalog.graph["resources"][piece["resource"]]
                missing[key] = {
                    "definition": definition, "interval": interval, "phase": phase, "status": status,
                    "implementation": owner["implementation"], "scope": owner["role"], "kind": kind,
                    "resource": resource["role"], "work": 0,
                    "axes": {name: [value, value] for name, value in features["axes"].items()},
                }
            group = missing[key]
            group["work"] += 1
            for name, value in features["axes"].items():
                lo, hi = group["axes"].get(name, (value, value))
                group["axes"][name] = [min(lo, value), max(hi, value)]
        gaps = sorted(missing.values(), key=lambda group: group["work"], reverse=True)
        return {"identities": str(identities_path.resolve()), "counts": counts, "gaps": gaps}

    def document(self, domain):
        return {
            "domain": domain, "definitions": self.definitions,
            "models": [{"definition": definition, "interval": interval, "phase": phase, **model}
                       for (definition, interval, phase), model in self.curves.items()],
        }


class FitCosts:
    def __init__(self, arguments):
        self.arguments = arguments

    @staticmethod
    def read(path):
        return json.loads(path.read_text())

    def run(self):
        sources = [self.read(path) for path in self.arguments.samples]
        grouped = defaultdict(list)
        for source in sources:
            grouped[DomainCosts.identity(source["domain"])].append(source)
        domains = {key: DomainCosts(rows) for key, rows in grouped.items()}
        absent = DomainCosts(())

        # A request in another domain is an uncovered demand, even when its definitions look identical.
        checks = []
        for path in self.arguments.check:
            source = self.read(path)
            costs = domains.get(DomainCosts.identity(source["domain"]), absent)
            checks.append({"source": str(path.resolve()), **costs.evaluate(source)})
        demand = []
        for graph_path, identities_path in self.arguments.demand:
            catalog = WorkCatalog(graph_path, identities_path)
            costs = domains.get(DomainCosts.identity(catalog.domain), absent)
            demand.append(costs.demand(catalog, identities_path))

        write_json(self.arguments.output, {
            "domains": [costs.document(grouped[key][0]["domain"]) for key, costs in domains.items()],
            "sources": [str(path.resolve()) for path in self.arguments.samples],
            "source_gaps": [source["unmeasured"] for source in sources],
            "checks": checks,
            "demand": demand,
        }, replace=False)
