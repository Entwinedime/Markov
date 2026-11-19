"""Fit a bounded service model to independent source samples."""

from collections import Counter, defaultdict
from contextlib import nullcontext
import json
from statistics import median

from markov_adapters.artifacts import write_json
from markov_workflow.adapters.sglang_cost_reuse import BatchHostReuse
from markov_workflow.containers import ModelingProcess
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


class DomainCosts:
    def __init__(self, sources):
        self.definitions = []
        indices_by_definition = {}
        groups = defaultdict(list)
        for source in sources:
            indices = []
            for definition in source["definitions"]:
                key = cost_identity(definition)
                if key not in indices_by_definition:
                    indices_by_definition[key] = len(self.definitions)
                    self.definitions.append(definition)
                indices.append(indices_by_definition[key])
            for row in source["samples"]:
                groups[indices[row["definition"]], row["interval"], row["features"]["phase"]].append(row)

        self.curves = {key: ServiceCurve(rows).document() for key, rows in sorted(groups.items())}

    def document(self, domain):
        return {
            "domain": domain, "definitions": self.definitions,
            "models": [{"definition": definition, "interval": interval, "phase": phase, **model}
                       for (definition, interval, phase), model in self.curves.items()],
        }


class FittedCosts:
    """Fit and publish the selected domains, using the modeling engine to qualify coverage."""

    def __init__(self, sources):
        grouped = defaultdict(list)
        for source in sources:
            grouped[cost_identity(source["domain"])].append(source)
        self.domains = [DomainCosts(rows).document(rows[0]["domain"]) for rows in grouped.values()]

    @classmethod
    def qualify(cls, sources, demand, model, directory, accounting=nullcontext):
        with accounting():
            fitted = cls(sources.values())
        counts, missing = demand.check(model, fitted.domains, directory, accounting)
        with accounting():
            reused = BatchHostReuse.match(demand, missing, sources)
            if reused:
                fitted = cls([*sources.values(), *reused])
        if reused:
            counts, missing = demand.check(model, fitted.domains, directory, accounting)
        return fitted, reused, counts, missing


class FitCosts:
    def __init__(self, arguments):
        self.arguments = arguments

    @staticmethod
    def read(path):
        return json.loads(path.read_text())

    @staticmethod
    def assess(source, estimates):
        counts, errors = defaultdict(Counter), defaultdict(list)
        for row, (status, estimate) in zip(source["samples"], estimates, strict=True):
            kind = source["definitions"][row["definition"]]["work"][row["interval"]]["kind"]
            observations = row["observations_ns"]
            counts[kind][status] += len(observations)
            if estimate is not None and (actual := median(observations)) > 0:
                errors[kind].append(abs(estimate - actual) / actual)
        return {
            "counts": counts,
            "median_point_relative_error": {kind: median(values) for kind, values in sorted(errors.items())},
            "unmeasured": source["unmeasured"],
        }

    def run(self):
        sources = {path.resolve(strict=True): self.read(path) for path in self.arguments.samples}
        demand = CostRequirements()
        for graph_path, identities_path in self.arguments.demand:
            demand.graph(str(graph_path.resolve()), WorkCatalog(graph_path, identities_path))
        for path in self.arguments.gap:
            demand.gap(str(path.resolve()), self.read(path))

        checks = CostRequirements()
        check_sources = []
        for path in self.arguments.check:
            source = self.read(path)
            name = str(path.resolve())
            check_sources.append((name, source, checks.samples(name, source)))

        directory = self.arguments.output.parent.resolve()
        model = None
        if demand.points or checks.points:
            if self.arguments.binary is None:
                raise ValueError("cost demand and sample checks require --binary")
            directory.mkdir(parents=True, exist_ok=True)
            model = ModelingProcess(self.arguments.binary, self.arguments.modeling_image, self.arguments.timeout)

        fitted, reused, counts, missing = FittedCosts.qualify(sources, demand, model, directory)
        estimates = checks.query(model, fitted.domains, directory)
        evaluations = [{"source": name, **self.assess(source, (estimates[point] for point in points))}
                       for name, source, points in check_sources]

        write_json(self.arguments.output, {
            "domains": fitted.domains,
            "sources": [str(path) for path in sources],
            "source_gaps": [source["unmeasured"] for source in sources.values()],
            "reused": reused,
            "checks": evaluations,
            "demand": demand.report(counts, missing),
        }, replace=False)
