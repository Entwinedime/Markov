"""Materialize declared source probes or held-out requests after an investigation freeze."""

import argparse
import hashlib
import json
from pathlib import Path

from markov_adapters.artifacts import write_json
from markov_adapters.sglang.configuration import CaptureInput
from markov_workload import WorkloadContent
from markov_workload.expert_routes import ExpertRoutes


class MatrixInputs:
    def __init__(self, root, specification, stage, seed):
        self.root = root.resolve(strict=True)
        self.specification = json.loads(specification.read_text())
        self.stage, self.seed = stage, seed
        self.domains = {row["id"]: row for row in self.specification["domains"] if row["stage"] == stage}
        self.cases = [row for row in self.specification["cases"] if row["domain"] in self.domains]
        if not self.cases:
            raise ValueError("the specification has no cases for this stage")

    def publish(self, name, document):
        path = (self.root / name).resolve()
        if not path.is_relative_to(self.root):
            raise ValueError("prepared inputs must remain inside their investigation directory")
        if path.exists():
            if json.loads(path.read_text()) != document:
                raise ValueError(f"an already published input would change: {path}")
            return
        write_json(path, document, indent=2, replace=False)

    def choose(self, case, request, field):
        choices = case[field]
        key = "\n".join((self.seed, case["id"], str(request), field)).encode()
        return choices[int.from_bytes(hashlib.sha256(key).digest(), "big") % len(choices)]

    @staticmethod
    def routes(requests, prefixes, geometry, mode):
        content = WorkloadContent(requests, prefixes)
        experts, top_k = geometry["experts"], geometry["top_k"]
        if mode not in {"balanced", "concentrated", "alternating"}:
            raise ValueError("unknown declared expert-route pattern")
        assignments = []
        for index, request in enumerate(requests):
            length = request["prompt_tokens"] + request["output_tokens"] - 1
            prefix = content.lengths[request.get("prefix_id")]
            cuts = {0, prefix, length}
            if mode == "alternating":
                cuts.add(max(prefix, length // 2))
            cuts = sorted(cuts)
            for begin, end in zip(cuts, cuts[1:]):

                # Shared prefix tokens keep identical logical choices; private suffixes can change their distribution.
                balanced = begin < prefix or mode == "balanced" or (mode == "alternating" and begin < length // 2)
                step = experts // top_k if balanced else 1
                offset = 0 if begin < prefix else index * 13
                assignments.append({
                    "request": request["id"], "layers": [0, geometry["layers"]], "tokens": [begin, end],
                    "experts": [(offset + expert * step) % experts for expert in range(top_k)],
                })
        return assignments

    def workload(self, case, instances, geometry):
        requests = []
        for index in range(case["requests"]):
            request = {
                "id": f"req-{index:04d}", "arrival_ns": index // case["arrival_group"] * case["arrival_gap_ns"],
                "prompt_tokens": self.choose(case, index, "prompt_tokens"),
                "output_tokens": self.choose(case, index, "output_tokens"),
            }
            prefix = case["prefix_ids"][index % len(case["prefix_ids"])]
            if prefix is not None:
                request["prefix_id"] = prefix
            requests.append(request)
        result = {"id": case["id"], "requests": requests, "prefixes": case["prefixes"]}
        WorkloadContent(requests, case["prefixes"])
        topologies = {instance["configuration"]["ep"]["id"]: instance["configuration"]["ep"]["size"]
                      for instance in instances if "ep" in instance["configuration"]}
        if topologies:
            assignments = self.routes(requests, case["prefixes"], geometry, case["routes"])
            balanced = [expert * (geometry["experts"] // geometry["top_k"]) for expert in range(geometry["top_k"])]
            result["ep_routes"] = {}
            layers = [{"layer": layer, "experts": geometry["experts"], "top_k": geometry["top_k"]}
                      for layer in range(geometry["layers"])]
            for identity, size in topologies.items():
                route = {"assignments": assignments, "padding": [] if size == 1 else [{
                    "layers": [0, geometry["layers"]], "tokens": [0, size - 1], "experts": balanced,
                }]}
                ExpertRoutes(route, requests, layers, geometry["layers"], size - 1)
                result["ep_routes"][identity] = route
        return result

    def run(self, phase):
        models = self.specification["models"]
        if phase == "targets":
            for case in self.cases:
                domain = self.domains[case["domain"]]
                model = models[domain["model"]]
                configuration = next(row for row in domain["configurations"] if row["id"] == case["configuration"])
                workload = self.workload(case, configuration["instances"], model["geometry"])
                common = {"model": model["model"], "instances": configuration["instances"], "workload": workload}
                native = {**common, "observe_all_workers": True}
                CaptureInput(native)
                self.publish(case["capture_input"], native)
                self.publish(case["prediction_input"], {**common, "given_work_ns": 1})
            self.publish(f"inputs/independent/{self.stage}/draw.json", {
                "seed": self.seed, "cases": [case["id"] for case in self.cases],
                "selection": self.specification["sampling"],
            })
            return

        for domain in self.domains.values():
            workloads = domain["source_workloads"]
            if not workloads:
                workloads = [json.loads((self.root / case["capture_input"]).read_text())["workload"]
                             for case in self.cases if case["domain"] == domain["id"]]

            # Explicit source probes stay independent of drawn targets; ordinary exploration sees its actual requested inputs.
            self.publish(domain["exploration"], {
                "framework": "sglang", "model": models[domain["model"]]["model"],
                "configurations": domain["configurations"], "workloads": workloads,
            })


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--specification", type=Path, required=True)
    parser.add_argument("--stage", required=True)
    parser.add_argument("--seed", required=True, help="final freeze delivery commit, retained verbatim in draw.json")
    parser.add_argument("--phase", choices=("sources", "targets"), required=True)
    arguments = parser.parse_args()
    MatrixInputs(arguments.root, arguments.specification, arguments.stage, arguments.seed).run(arguments.phase)


if __name__ == "__main__":
    main()
