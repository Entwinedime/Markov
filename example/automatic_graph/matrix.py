"""Prepare finite experiment inputs; this script never loads models or executes cases."""

import argparse
import copy
import json
import random
from pathlib import Path


class MatrixInputs:
    def __init__(self, directory):
        self.directory = directory
        self.manifest = json.loads((directory / "inputs/matrix.json").read_text())
        if self.manifest["status"] != "preparing":
            raise ValueError("matrix inputs can only be generated before freezing")
        self.models = {model["id"]: model for model in self.manifest["models"]}
        self.durations = json.loads(Path("example/automatic_graph/exploration.json").read_text())["node_durations_ns"]
        self.tasks = {}

    def write(self, name, value):
        path = self.directory / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value, indent=2) + "\n")
        return name

    def requests(self, page, count=8, seed=None):
        generator = random.Random(self.manifest["request_seed"] + (seed or 0))
        prompts = [
            1,
            max(1, page - 1),
            page,
            page + 1,
            2 * page - 1,
            2 * page,
            2 * page + 1,
            3 * page + generator.randint(1, max(1, page // 2)),
        ]
        outputs = [2, 1, 6, 3, 2, 4, 1, 3]

        # Freeze fresh request combinations while retaining the named page boundaries.
        generator.shuffle(prompts)
        generator.shuffle(outputs)
        result = []
        for index in range(count):
            prompt = (
                prompts[index % len(prompts)] if seed is None else generator.choice([3, 23, page + 11, 2 * page + 13])
            )
            output = outputs[index % len(outputs)] if seed is None else generator.choice([1, 2, 5])
            result.append({"id": f"request-{index}", "arrival_ns": 0, "prompt_tokens": prompt, "output_tokens": output})
        return result

    def routes(self, model, requests, size, mode):
        layers, experts, top_k = (model[key] for key in ("num_hidden_layers", "num_experts", "num_experts_per_tok"))
        assignments = []
        padding = []
        for begin, end in ((0, layers // 2), (layers // 2, layers)):
            hot = list(range(top_k)) if begin == 0 else list(range(experts - top_k, experts))
            for index, request in enumerate(requests):
                total = request["prompt_tokens"] + request["output_tokens"] - 1
                if mode == "hotspot" or begin == 0:
                    parts = [(0, total, hot)]
                else:
                    balanced = [(expert * experts // top_k + index) % experts for expert in range(top_k)]
                    cut = request["prompt_tokens"]
                    parts = [(0, cut, balanced)]
                    if cut < total:
                        parts.append((cut, total, hot))
                for first, last, choices in parts:
                    assignments.append(
                        {"request": request["id"], "layers": [begin, end], "tokens": [first, last], "experts": choices}
                    )
            if size > 1:
                padding.append({"layers": [begin, end], "tokens": [0, size - 1], "experts": hot})
        return {"assignments": assignments, "padding": padding}

    def job(self, identifier, model_id, tp=1, pp=1, ep=1, page=128, dtype="bfloat16", pd=None, groups=None):
        model = self.models[model_id]
        moe = "num_experts" in model
        configurations = []
        for name, running, tokens, microbatch in (("small", 3, 4096, 1), ("wide", 64, 65536, 3)):
            configuration = {"id": name, "attention_backend": "ascend", "page_size": page}
            pools = {"prefill": pd[0], "decode": pd[1]} if pd else {"model": (tp, pp, ep)}
            converted = {}
            for role, (pool_tp, pool_pp, pool_ep) in pools.items():
                pool = {"tp": pool_tp, "max_running_requests": running, "max_total_tokens": tokens}
                if role != "decode":
                    pool["max_prefill_tokens"] = 4096
                if pool_pp > 1:
                    pool["pp"] = pool_pp
                    if not pd:
                        pool["microbatch_requests"] = microbatch
                if moe:
                    pool["ep"] = {"id": f"{role}-experts", "size": pool_ep}
                converted[role] = pool
            if pd:
                configuration["pd"] = converted
            else:
                configuration.update(converted["model"])
            configurations.append(configuration)

        if pp > 1 and not pd:
            configuration = copy.deepcopy(configurations[-1])
            configuration.update(id="full-micro", microbatch_requests=8)
            configurations.append(configuration)

        requests = self.requests(page)
        workloads = []
        for name in ("varying", "hotspot") if moe else ("boundaries", "staggered"):
            workload = {"id": name, "requests": copy.deepcopy(requests)}
            if moe:
                workload["ep_routes"] = {}
                for role, (_, _, pool_ep) in pools.items():
                    workload["ep_routes"][f"{role}-experts"] = self.routes(model, requests, pool_ep, name)
            elif name == "staggered":
                for index, request in enumerate(workload["requests"]):
                    request["arrival_ns"] = (index // 2) * 230000 + (1100000 if index > 4 else 0)
            workloads.append(workload)

        devices = sum(pool_tp * pool_pp for pool_tp, pool_pp, _ in pools.values())
        source_sessions = 1 if pd or pp > 1 or ep > 1 else tp
        native_cases = [{"configuration": "wide", "workload": "varying" if moe else "boundaries"}]
        task = {
            "framework": "sglang",
            "model": {"path": model["path"], "dtype": dtype},
            "deployment": {"physical_devices": list(range(devices))},
            "budget": {
                "native_seconds": 600 * source_sessions + 700,
                "processing_seconds": 3600 if pd else 2400 if moe else 1800,
                "temporary_bytes": (20 if moe or pd else 12) * 1024**3,
                "retained_bytes": (8 if moe or pd else 4) * 1024**3,
            },
            "configurations": configurations,
            "workloads": workloads,
            "node_durations_ns": self.durations,
        }
        self.tasks[identifier] = task
        entry = {
            "id": identifier,
            "model": model_id,
            "task": f"runs/{identifier}/inputs/task.json",
            "groups": groups or ["B", "F"],
            "native_cases": native_cases,
            "native_attempt_cap": source_sessions + len(native_cases),
            "expected": "investigate",
        }
        self.manifest["jobs"].append(entry)
        return task, entry

    def prepare(self):
        self.manifest["jobs"] = []
        self.manifest["conditional_cases"] = []
        for tp in (1, 2, 4, 8):
            self.job(f"qwen3-tp{tp}", "qwen3-8b", tp=tp, groups=["A", "D", "E", "F", "G", "H", "L"])
        for tp, pp in ((1, 3), (1, 4), (2, 3), (2, 4)):
            self.job(f"qwen3-tp{tp}-pp{pp}", "qwen3-8b", tp=tp, pp=pp, groups=["F", "H", "K"])
        for tp, pp in ((1, 1), (4, 1), (1, 3), (1, 4)):
            self.job(f"mistral-tp{tp}-pp{pp}", "mistral-7b", tp=tp, pp=pp, groups=["B", "F", "H"])
        for ep in (1, 2, 4):
            self.job(f"qwen15-moe-ep{ep}", "qwen15-moe", tp=ep, ep=ep, groups=["B", "I"])
        for ep in (2, 4, 8):
            self.job(f"qwen3-moe-ep{ep}", "qwen3-moe", tp=ep, ep=ep, groups=["B", "H", "I"])
        self.job("qwen3-moe-ep2-pp3", "qwen3-moe", tp=2, pp=3, ep=2, groups=["H", "I", "K"])
        self.job("qwen3-moe-pd2-2", "qwen3-moe", pd=((2, 1, 2), (2, 1, 2)), groups=["I", "J", "K"])
        for prefill, decode in ((4, 2), (2, 4)):
            self.job(f"qwen3-pd{prefill}-{decode}", "qwen3-8b", pd=((prefill, 1, 1), (decode, 1, 1)), groups=["J", "K"])
        self.job("qwen3-moe-pd-pp3", "qwen3-moe", pd=((2, 3, 2), (2, 1, 2)), groups=["H", "I", "J", "K"])
        for tp in (1, 2):
            self.job(f"llama31-tp{tp}", "llama31-8b", tp=tp, groups=["B", "F", "H"])

        # Native OLMoE TP/PP failures remain in the first investigation report.
        self.job("olmoe-ep1", "olmoe", groups=["B", "I"])
        self.job("qwen3-fp16", "qwen3-8b", dtype="float16", groups=["B"])
        for page in (1, 16, 64, 256):
            self.job(f"qwen3-page{page}", "qwen3-8b", page=page, groups=["C", "F"])
        self.job("qwen3-page64-pp3", "qwen3-8b", pp=3, page=64, groups=["C", "H"])
        self.job("qwen3-moe-page64-ep2", "qwen3-moe", tp=2, ep=2, page=64, groups=["C", "I"])
        self.job("qwen3-page64-pd1-1", "qwen3-8b", pd=((1, 1, 1), (1, 1, 1)), page=64, groups=["C", "J"])
        for entry in self.manifest["jobs"]:
            if entry["id"] == "qwen3-tp2-pp4":
                entry["native_cases"][0]["configuration"] = "full-micro"
            elif entry["id"] == "mistral-tp1-pp3":
                entry["native_cases"][0]["configuration"] = "small"
        self.extended_inputs()
        self.capacity_and_route_inputs()
        self.lifecycle_inputs()
        for entry in self.manifest["jobs"]:
            self.write(entry["task"], self.tasks[entry["id"]])
        self.manifest["round_budget"] = {
            "native_attempts": sum(entry["native_attempt_cap"] for entry in self.manifest["jobs"]),
            "native_seconds": 24000,
            "processing_seconds": 28800,
            "temporary_bytes": 120 * 1024**3,
            "retained_bytes": 96 * 1024**3,
            "concurrent_jobs": 1,
        }
        self.manifest["preparation_remaining"] = [
            "Complete development preflight of budgeted execution, recovery and reporting",
            "Complete final code/document/data checks",
            "Record and freeze the reviewed code and all case inputs before execution",
        ]
        self.write("inputs/matrix.json", self.manifest)

    def extended_inputs(self):
        task = self.tasks["qwen3-tp1"]
        entry = next(item for item in self.manifest["jobs"] if item["id"] == "qwen3-tp1")
        for running in (1, 2, 31, 32, 33, 101, 128):
            configuration = copy.deepcopy(task["configurations"][1])
            configuration.update(id=f"r{running}", max_running_requests=running)
            task["configurations"].append(configuration)
        for count in (31, 32, 33, 64, 101, 128):
            requests = [
                {"id": f"member-{index}", "arrival_ns": 0, "prompt_tokens": 1 + index % 3, "output_tokens": 3}
                for index in range(count)
            ]
            task["workloads"].append({"id": f"members-{count}", "requests": requests})
        for count, seed in ((64, 2701), (128, 2702), (256, 2703)):
            requests = self.requests(128, count, seed)
            for index, request in enumerate(requests):
                request["arrival_ns"] = (index // 7) * 37000 + (900000 if index >= count // 2 else 0)
            task["workloads"].append({"id": f"seed-{count}", "requests": requests})
        task["workloads"].append(
            {
                "id": "long-outputs",
                "requests": [
                    {"id": "long-first", "arrival_ns": 0, "prompt_tokens": 23, "output_tokens": 32},
                    {"id": "one-step", "arrival_ns": 0, "prompt_tokens": 137, "output_tokens": 1},
                    {"id": "long-last", "arrival_ns": 0, "prompt_tokens": 263, "output_tokens": 64},
                ],
            }
        )
        entry["native_cases"] = [
            {"configuration": "small", "workload": "boundaries"},
            {"configuration": "r32", "workload": "members-32", "minimum_batch_members": 32},
            {"configuration": "r33", "workload": "members-33", "minimum_batch_members": 33},
            {"configuration": "r128", "workload": "members-128", "minimum_batch_members": 128},
            {"configuration": "wide", "workload": "long-outputs"},
        ]
        entry["native_attempt_cap"] = 1 + len(entry["native_cases"])
        task["budget"].update(native_seconds=4500, processing_seconds=5400, retained_bytes=8 * 1024**3)

        # Keep broad request/capacity boundaries without multiplying every single-axis case.
        for running in (1, 2, 31, 64, 101):
            configuration = copy.deepcopy(task["configurations"][1])
            configuration["max_running_requests"] = running
            workload = (
                task["workloads"][0]
                if running < 3
                else next(item for item in task["workloads"] if item["id"] == f"members-{running}")
            )
            self.conditional(
                f"D-running-{running}",
                "qwen3-tp1",
                self.target(configuration, workload, self.durations),
                "complete",
                ["D"],
            )
        for count, running in ((64, 32), (128, 64), (256, 128)):
            configuration = copy.deepcopy(task["configurations"][1])
            configuration["max_running_requests"] = running
            workload = next(item for item in task["workloads"] if item["id"] == f"seed-{count}")
            self.conditional(
                f"G-seed-{count}", "qwen3-tp1", self.target(configuration, workload, self.durations), "complete", ["G"]
            )
        task["configurations"] = [
            item for item in task["configurations"] if item["id"] in ("small", "wide", "r32", "r33", "r128")
        ]
        task["workloads"] = [
            item
            for item in task["workloads"]
            if item["id"] in ("boundaries", "staggered", "members-32", "members-33", "members-128", "long-outputs")
        ]

        long_context = {
            "id": "context-boundaries",
            "requests": [
                {"id": f"context-{length}", "arrival_ns": 0, "prompt_tokens": length, "output_tokens": 2}
                for length in (2047, 2048, 2049, 2305)
            ],
        }
        for identifier in ("qwen3-tp2", "llama31-tp1", "llama31-tp2"):
            self.tasks[identifier]["workloads"].append(copy.deepcopy(long_context))
        for tokens in (16384, 262144):
            configuration = copy.deepcopy(self.tasks["qwen3-tp2"]["configurations"][1])
            configuration.update(id=f"kv{tokens}", max_total_tokens=tokens)
            self.tasks["qwen3-tp2"]["configurations"].append(configuration)
        entry = next(item for item in self.manifest["jobs"] if item["id"] == "qwen3-tp2")
        entry["native_cases"].append({"configuration": "kv262144", "workload": "context-boundaries"})
        entry["native_attempt_cap"] += 1
        self.tasks["qwen3-tp2"]["budget"]["native_seconds"] += 700

        for identifier in ("qwen3-tp4", "qwen3-moe-pd-pp3"):
            task = self.tasks[identifier]
            configuration = task["configurations"][1]
            workload = task["workloads"][0]
            for category, duration in (("host", 137), ("device", 4009), ("communication", 31003), ("transfer", 700001)):
                target = self.target(configuration, workload, task["node_durations_ns"])
                target["node_durations_ns"][category] = duration
                self.conditional(f"K-{identifier}-{category}", identifier, target, "complete", ["K"])

        base = self.tasks["qwen3-tp1"]
        target = self.target(base["configurations"][0], base["workloads"][0], self.durations)
        negative = copy.deepcopy(target)
        negative["configuration"].update(max_running_requests=1, max_total_tokens=128)
        negative["requests"] = [{"id": "too-large", "arrival_ns": 0, "prompt_tokens": 257, "output_tokens": 2}]
        self.conditional("E-request-exceeds-pool", "qwen3-tp1", negative, "rejected", ["E"])
        for identifier, field, value in (("A-zero-prompt", "prompt_tokens", 0), ("A-type", "prompt_tokens", "5")):
            negative = copy.deepcopy(target)
            negative["requests"][0][field] = value
            self.conditional(identifier, "qwen3-tp1", negative, "rejected", ["A"])
        negative = copy.deepcopy(target)
        negative["requests"][1]["id"] = negative["requests"][0]["id"]
        self.conditional("A-duplicate-request", "qwen3-tp1", negative, "rejected", ["A"])
        negative = copy.deepcopy(target)
        negative["configuration"]["page_size"] = 64
        self.conditional("C-wrong-page-asset", "qwen3-tp1", negative, "rejected", ["C"])
        negative = copy.deepcopy(target)
        negative["configuration"].update(pp=3, microbatch_requests=2)
        self.conditional("A-wrong-topology-asset", "qwen3-tp1", negative, "rejected", ["A", "H"])

    @staticmethod
    def target(configuration, workload, durations):
        target = {
            "configuration": {
                key: value for key, value in configuration.items() if key not in ("id", "tp", "attention_backend")
            },
            "requests": copy.deepcopy(workload["requests"]),
            "node_durations_ns": copy.deepcopy(durations),
        }
        if "ep_routes" in workload:
            target["ep_routes"] = copy.deepcopy(workload["ep_routes"])
        return target

    def capacity_and_route_inputs(self):
        common = {
            "id": "unit-capacity",
            "requests": [
                {"id": f"unit-{length}", "arrival_ns": 0, "prompt_tokens": length, "output_tokens": 2}
                for length in (5, 129, 257)
            ],
        }
        for page in (1, 16, 64, 128, 256):
            identifier = "qwen3-tp1" if page == 128 else f"qwen3-page{page}"
            task = self.tasks[identifier]
            if page != 128:
                task["workloads"].append(copy.deepcopy(common))
                entry = next(job for job in self.manifest["jobs"] if job["id"] == identifier)
                entry["native_cases"] = [{"configuration": "small", "workload": "unit-capacity"}]
            configuration = copy.deepcopy(task["configurations"][0])
            configuration["max_total_tokens"] = 32 * page
            workload = {
                "id": "equal-pages",
                "requests": [
                    {"id": "page-before", "arrival_ns": 0, "prompt_tokens": max(1, page - 1), "output_tokens": 2},
                    {"id": "page-after", "arrival_ns": 0, "prompt_tokens": page + 1, "output_tokens": 3},
                ],
            }
            self.conditional(
                f"C-equal-pages-{page}",
                identifier,
                self.target(configuration, workload, self.durations),
                "complete",
                ["C", "E"],
            )

        task = self.tasks["qwen3-moe-ep2"]
        target = self.target(task["configurations"][0], task["workloads"][0], self.durations)
        missing = copy.deepcopy(target)
        del missing["ep_routes"]
        self.conditional("A-missing-routes", "qwen3-moe-ep2", missing, "rejected", ["A", "I"])
        duplicate = copy.deepcopy(target)
        experts = duplicate["ep_routes"]["model-experts"]["assignments"][0]["experts"]
        experts[1] = experts[0]
        self.conditional("A-duplicate-expert", "qwen3-moe-ep2", duplicate, "rejected", ["A", "I"])
        gap = copy.deepcopy(target)
        gap["ep_routes"]["model-experts"]["assignments"].pop(0)
        self.conditional("A-route-gap", "qwen3-moe-ep2", gap, "rejected", ["A", "I"])

    def lifecycle_inputs(self):
        base = copy.deepcopy(self.tasks["qwen3-tp1"])
        base["configurations"] = [copy.deepcopy(base["configurations"][0])]
        base["workloads"] = [copy.deepcopy(base["workloads"][0])]
        invalid = copy.deepcopy(base)
        invalid["framework"] = "unsupported"
        invalid_path = self.write("lifecycle/invalid/inputs/task.json", invalid)
        budget = copy.deepcopy(base)
        budget["budget"].update(native_seconds=0, processing_seconds=0)
        budget_path = self.write("lifecycle/budget/inputs/task.json", budget)
        interrupted = copy.deepcopy(base)
        interrupted["configurations"][0].update(id="only", max_running_requests=1, max_total_tokens=65536)
        interrupted["workloads"] = [
            {
                "id": "interrupted",
                "requests": [
                    {"id": f"resume-{index}", "arrival_ns": 0, "prompt_tokens": 3 + index % 3, "output_tokens": 8}
                    for index in range(256)
                ],
            }
        ]
        interrupted["budget"].update(
            native_seconds=0, processing_seconds=300, temporary_bytes=2 * 1024**3, retained_bytes=32 * 1024**2
        )
        interrupted_path = self.write("lifecycle/interrupted/inputs/task.json", interrupted)
        guarded = self.write(
            "inputs/cases/L-protect-input.json",
            self.target(base["configurations"][0], base["workloads"][0], self.durations),
        )
        self.manifest["lifecycle_cases"] = [
            {"id": "L-cli-invalid", "kind": "reject-task", "input": invalid_path, "groups": ["A", "L"]},
            {"id": "L-help-after-error", "kind": "help", "groups": ["L"]},
            {"id": "L-resume-completed", "kind": "resume-completed", "job": "qwen3-tp1", "groups": ["L"]},
            {"id": "L-budget-stop", "kind": "budget", "task": budget_path, "groups": ["L"]},
            {
                "id": "L-interrupt-resume",
                "kind": "interrupt",
                "job": "qwen3-tp1",
                "task": interrupted_path,
                "groups": ["L"],
            },
            {"id": "L-bind-input", "kind": "missing-input", "job": "qwen3-tp1", "groups": ["L"]},
            {"id": "L-protect-input", "kind": "protect-input", "job": "qwen3-tp1", "input": guarded, "groups": ["L"]},
        ]

    def conditional(self, identifier, job, target, expected, groups):
        name = self.write(f"inputs/cases/{identifier}.json", target)
        self.manifest["conditional_cases"].append(
            {"id": identifier, "job": job, "input": name, "expected": expected, "groups": groups}
        )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", type=Path, required=True)
    arguments = parser.parse_args()
    directory = arguments.run.resolve()
    directory.relative_to(Path.cwd() / "data")
    generator = MatrixInputs(directory)
    generator.prepare()
    print(
        json.dumps(
            {
                "jobs": len(generator.manifest["jobs"]),
                "conditional_cases": len(generator.manifest["conditional_cases"]),
                "prediction_pairs": sum(
                    len(task["configurations"]) * len(task["workloads"]) for task in generator.tasks.values()
                ),
                "native_cases": sum(len(job["native_cases"]) for job in generator.manifest["jobs"]),
                "round_budget": generator.manifest["round_budget"],
            }
        )
    )
