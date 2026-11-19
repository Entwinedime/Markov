"""Summarize request output publication on the simulation clock."""

from collections import defaultdict
from math import ceil
from statistics import mean


def percentile95(values):
    ordered = sorted(values)
    return ordered[ceil(len(ordered) * 0.95) - 1]


class PredictedLatency:
    def __init__(self, result, requests):
        self.result, self.requests = result, requests

    def report(self):
        result = self.result
        if result["duration_basis"] != "independent_service_costs" or result["requests"] != len(self.requests):
            raise ValueError("latency metrics require complete predictions driven by independent costs")

        published = defaultdict(list)
        for token in result["outputs"]:
            published[token["request"]].append(token)
        if set(published) != {row["id"] for row in self.requests}:
            raise ValueError("prediction output contains a different request set")

        requests, total = [], 0
        for request in self.requests:
            identity, count = request["id"], request["output_tokens"]
            tokens = published[identity]
            times = [token["time_ns"] for token in tokens]
            if (len(times) != count or times != sorted(times) or times[0] < request["arrival_ns"]
                    or any(row["generated_tokens"] != number or row["finished"] != (number == count)
                           for number, row in enumerate(tokens, 1))):
                raise ValueError(f"prediction has incomplete or unordered token publication for {identity}")
            requests.append({
                "id": identity, "ttft_ns": times[0] - request["arrival_ns"], "last_publication_ns": times[-1],
                "tpot_ns": (times[-1] - times[0]) / (count - 1) if count > 1 else None,
            })
            total += count
        elapsed = max(row["last_publication_ns"] for row in requests)
        per_token = [row["tpot_ns"] for row in requests if row["tpot_ns"] is not None]
        return {
            "requests": requests, "output_tokens": total, "p95_ttft_ns": percentile95(row["ttft_ns"] for row in requests),
            "mean_tpot_ns": mean(per_token) if per_token else None, "tokens_per_second": total * 1e9 / elapsed if elapsed else None,
        }
