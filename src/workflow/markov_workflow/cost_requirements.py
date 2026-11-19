"""Collect the same work coordinates from projected graphs and prediction gaps."""

import json
from collections import Counter, defaultdict
from contextlib import nullcontext
from dataclasses import dataclass
from pathlib import Path
from tempfile import TemporaryDirectory

from markov_adapters.artifacts import write_json


def cost_identity(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)


@dataclass(frozen=True, slots=True)
class WorkPoint:
    domain: str
    definition: str
    interval: int
    phase: str
    axes: tuple

    @classmethod
    def at(cls, domain, definition, interval, features):
        return cls(domain, definition, interval, features["phase"], tuple(sorted(features["axes"].items())))

    def features(self):
        return {"phase": self.phase, "axes": dict(self.axes)}


class CostRequirements:
    """Share identical observed points while retaining the targets that need them."""

    def __init__(self):
        self.domains = {}
        self.definitions = {}
        self.targets = {}
        self.points = defaultdict(Counter)

    @staticmethod
    def intern(table, value):
        identity = cost_identity(value)
        table.setdefault(identity, value)
        return identity

    def add(self, target, domain, definition, interval, features):
        point = WorkPoint.at(domain, definition, interval, features)
        self.points[point][target] += 1
        return point

    def graph(self, target, catalog):
        self.targets[target] = "observed_graph"
        domain = self.intern(self.domains, catalog.domain)
        identities = [self.intern(self.definitions, value) for value in catalog.definitions]
        return {self.add(target, domain, identities[definition], interval, features)
                for _, definition, interval, features in catalog}

    def external(self, target, domain, identity, kind, features):
        domain = self.intern(self.domains, domain)
        definition = self.intern(self.definitions, {"external": identity, "work": [{"kind": kind}]})
        return self.add(target, domain, definition, 0, features)

    def gap(self, target, gap):
        self.targets[target] = "first_prediction_gap"
        domain = self.intern(self.domains, gap["domain"])
        definition = self.intern(self.definitions, gap["definition"])
        if gap["definition"]["work"][gap["interval"]]["kind"] != gap["work"]:
            raise ValueError("prediction gap changes the kind of its declared work interval")
        self.add(target, domain, definition, gap["interval"], gap["features"])

    def samples(self, target, source):
        domain = self.intern(self.domains, source["domain"])
        definitions = [self.intern(self.definitions, value) for value in source["definitions"]]
        return [self.add(target, domain, definitions[row["definition"]], row["interval"], row["features"])
                for row in source["samples"]]

    def query(self, model, domains, directory, accounting=nullcontext):
        if not self.points:
            return {}
        with TemporaryDirectory(prefix=".cost-points-", dir=directory) as scratch:
            scratch = Path(scratch)
            points_path, asset_path, result_path = (scratch / name for name in ("points.json", "costs.json", "estimates.json"))
            with accounting():
                groups = defaultdict(list)
                for point in self.points:
                    groups[point.domain].append(point)

                queries = []
                for domain, points in groups.items():
                    definitions = {}
                    rows = [[definitions.setdefault(point.definition, len(definitions)), point.interval, point.features()]
                            for point in points]
                    queries.append({
                        "domain": self.domains[domain], "definitions": [self.definitions[key] for key in definitions],
                        "points": rows,
                    })
                write_json(points_path, queries)
                write_json(asset_path, {"domains": domains})

            model.run_model("cost-points", result_path, points_path, asset_path)
            with accounting():
                estimates = json.loads(result_path.read_text())
                return {point: estimate for points, results in zip(groups.values(), estimates, strict=True)
                        for point, estimate in zip(points, results, strict=True)}

    def check(self, model, domains, directory, accounting):
        estimates = self.query(model, domains, directory, accounting)
        with accounting():
            counts = {target: Counter() for target in self.targets}
            missing = {}
            for point, targets in self.points.items():
                status, _ = estimates[point]
                for target, number in targets.items():
                    counts[target][status] += number
                if status != "covered":
                    missing[point] = status
            return counts, missing

    def report(self, counts, missing, reasons=None):
        domains = {}
        for point, status in missing.items():
            domain = domains.setdefault(point.domain, {"definitions": {}, "work": {}})
            definitions = domain["definitions"]
            index = definitions.setdefault(point.definition, len(definitions))
            key = index, point.interval, point.phase, status
            definition = self.definitions[point.definition]
            row = domain["work"].setdefault(key, {
                "definition": index, "interval": point.interval, "phase": point.phase, "status": status,
                "kind": definition["work"][point.interval]["kind"], "points": [],
            })
            value = {"axes": dict(point.axes), "targets": dict(self.points[point])}
            if reasons is not None:
                value["reason"] = reasons[point]
            row["points"].append(value)
        return {
            "targets": {target: {"basis": basis, "counts": dict(counts[target])} for target, basis in self.targets.items()},
            "uncovered": [
                {"domain": self.domains[domain], "definitions": [self.definitions[name] for name in group["definitions"]],
                 "work": list(group["work"].values())}
                for domain, group in domains.items()
            ],
            "scope": "Recorded graph and protocol work and each supplied first cost gap. Work on unreached control paths is unknown.",
        }
