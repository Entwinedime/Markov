"""Report native scheduler latency and compare independent predictions for ordinary TP1."""

import argparse
from collections import defaultdict
import json
from math import ceil
from pathlib import Path
from statistics import mean

from markov_adapters.artifacts import write_json


class NativeLatencyMetrics:
    def __init__(self, source):
        self.source = source
        instance, = self.read(source / "capture.json")["instances"]
        rank, = instance["native_participants"]
        if instance["mode"] != "null":
            raise ValueError("native latency reporting requires one ordinary worker")
        directory = source / "instances/0"
        session = self.read(directory / "session.json")
        self.requests = session["content"]["requests"]
        delivery, = self.read(directory / "request-delivery.json")["capture"]
        if delivery["clock"] != "monotonic_raw_ns":
            raise ValueError("native latency requires the RAW workload clock")
        self.origin_ns = delivery["origin_ns"]
        self.submissions = {index: row for row in delivery["submissions"] for index in row["requests"]}
        self.specializations = None
        if "latency" in instance:
            measurements = self.read(source / instance["latency"])
            if measurements["clock"] != delivery["clock"]:
                raise ValueError("native latency and delivery clocks differ")
            self.rows = {row["id"]: row for row in measurements["requests"]}
            self.specializations = measurements.get("specialization_loads")
            self.basis = "scheduler_send_return"
        else:
            raw = self.read(source / f"rank-{rank}.json")
            aliases = dict(zip(session["source_requests"], (request["id"] for request in self.requests), strict=True))
            self.rows = self.observed_publications(raw, aliases)
            self.basis = "detailed_scheduler_send_bracket_end"

    @staticmethod
    def read(path):
        return json.loads(path.read_text())

    @staticmethod
    def observed_publications(raw, aliases):
        host = {row["name"]: row for row in raw["host_observation"]["intervals"]}
        rows = {aliases[identity]: {"ingress_raw_ns": pull["received_raw_ns"]}
                for pull in raw["request_ingress"] for identity in pull["requests"]}
        for scope in raw["scopes"]:
            if not scope.get("publication"):
                continue
            marker = host[f"markov.scope/{scope['id']}"]
            finish = marker["end_raw_ns"]
            bracket = finish - marker["begin_raw_ns"]
            for item in scope["publication"]:
                row = rows[item["id"]]
                count = item["generated_tokens"]
                if count > row.get("output_tokens", 0):
                    row.setdefault("first_publication_raw_ns", finish)
                    row.setdefault("first_publication_bracket_ns", bracket)
                    row.update(last_publication_raw_ns=finish, last_publication_bracket_ns=bracket, output_tokens=count)
        return rows

    def report(self):
        requests = []
        for index, expected in enumerate(self.requests):
            identity = expected["id"]
            row = self.rows[identity]
            count = row["output_tokens"]
            declared = self.submissions[index]["scheduled_ns"]
            submitted = self.submissions[index]["submitted_ns"]
            arrival = row["ingress_raw_ns"] - self.origin_ns
            first = row["first_publication_raw_ns"] - self.origin_ns
            last = row["last_publication_raw_ns"] - self.origin_ns
            if count != expected["output_tokens"] or not 0 <= declared <= submitted <= arrival <= first <= last:
                raise ValueError(f"native request {identity} has inconsistent delivery, output length or time")
            requests.append({
                **row, "id": identity, "declared_ns": declared, "submitted_ns": submitted, "ingress_ns": arrival,
                "first_publication_ns": first, "last_publication_ns": last, "ttft_ns": first - submitted,
                "scheduler_ttft_ns": first - arrival,
                "tpot_ns": (last - first) / (count - 1) if count > 1 else None,
            })
        total = sum(row["output_tokens"] for row in requests)
        elapsed = max(row["last_publication_ns"] for row in requests)
        return {"capture": str(self.source), "publication_basis": self.basis, "origin_raw_ns": self.origin_ns,
                "ttft_basis": "request_submission_to_scheduler_publication",
                "output_tokens": total, "tokens_per_second": total * 1e9 / elapsed if elapsed else None,
                "p95_ttft_ns": percentile95(row["ttft_ns"] for row in requests), "requests": requests,
                "specialization_loads": self.specializations}


def percentile95(values):
    ordered = sorted(values)
    return ordered[ceil(len(ordered) * 0.95) - 1]


class LatencyComparison:
    def __init__(self, native, prediction):
        self.native = native
        self.path = prediction
        self.result = NativeLatencyMetrics.read(prediction)

    def report(self):
        result = self.result
        if result["duration_basis"] != "independent_service_costs" or result["requests"] != len(self.native["requests"]):
            raise ValueError("performance comparison needs complete predictions driven by independent costs")
        produced = defaultdict(list)
        for token in result["outputs"]:
            produced[token["request"]].append(token)
        if set(produced) != {row["id"] for row in self.native["requests"]}:
            raise ValueError("native and predicted requests differ")

        requests = []
        for truth in self.native["requests"]:
            identity = truth["id"]
            tokens = produced[identity]
            count = truth["output_tokens"]
            times = [token["time_ns"] for token in tokens]
            if (len(times) != count or times != sorted(times) or times[0] < truth["declared_ns"]
                    or any(row["generated_tokens"] != number or row["finished"] != (number == count)
                           for number, row in enumerate(tokens, 1))):
                raise ValueError(f"prediction has incomplete or unordered token publication for {identity}")
            requests.append({
                "id": identity, "ttft_ns": times[0] - truth["declared_ns"], "last_publication_ns": times[-1],
                "tpot_ns": (times[-1] - times[0]) / (count - 1) if count > 1 else None,
            })
        elapsed = max(row["last_publication_ns"] for row in requests)
        accuracy = {}
        for metric in ("ttft_ns", "tpot_ns"):
            actual = [row[metric] for row in self.native["requests"]]
            errors = [abs(row[metric] - observed) / observed for row, observed in zip(requests, actual, strict=True) if observed]
            accuracy[metric] = {"mape_percent": mean(errors) * 100 if errors else None,
                                "requests": len(errors), "zero_denominators": actual.count(0)}
        return {"result": str(self.path), "requests": requests, "accuracy": accuracy,
                "p95_ttft_ns": percentile95(row["ttft_ns"] for row in requests),
                "tokens_per_second": self.native["output_tokens"] * 1e9 / elapsed if elapsed else None,
                "cost_queries": result["cost_queries"], "cost_query_ns": result["cost_query_ns"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", type=Path, nargs="+", required=True)
    parser.add_argument("--prediction", type=Path, nargs="+", help="independent result JSONs in the capture order")
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    rows = [NativeLatencyMetrics(path).report() for path in arguments.capture]
    if arguments.prediction is not None:
        for row, prediction in zip(rows, arguments.prediction, strict=True):
            row["prediction"] = LatencyComparison(row, prediction).report()
    write_json(arguments.output, rows, indent=2, replace=False)


if __name__ == "__main__":
    main()
