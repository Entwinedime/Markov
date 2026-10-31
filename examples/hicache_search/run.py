"""Enumerate a bounded HiCache space, predict once, then confirm three fixed roles."""

from __future__ import annotations

import argparse
from dataclasses import replace
import fcntl
from itertools import product
import json
from pathlib import Path

from markov.artifacts.request import PredictionRequest, read_json
from markov.artifacts.writing import write_json
from markov.workflow.capture import InferenceCapture
from markov.workflow.device_task import DeviceTask
from markov.workflow.search import SearchPolicy, search

from report import confirmation_names, write_report


EXAMPLE = Path(__file__).resolve().parent


def candidates(settings: dict) -> list[dict]:
    axes = settings["axes"]
    reference = {
        **settings["base_configuration"],
        "hicache_storage_backend_extra_config": json.loads(
            settings["base_configuration"]["hicache_storage_backend_extra_config"]
        ),
    }
    result = []
    for device, host, page, write, threshold, policy in product(
        axes["device_tokens"],
        axes["host_ratios"],
        axes["page_tokens"],
        axes["write_policies"],
        axes["prefetch_threshold_tokens"],
        axes["prefetch_policies"],
    ):
        for timeout in axes["timeout_sec"] if policy == "timeout" else [None]:
            extra = {"prefetch_threshold": threshold}
            if timeout is not None:
                extra.update(
                    prefetch_timeout_base=timeout, prefetch_timeout_per_ki_token=0.0, prefetch_timeout_max=timeout
                )
            configuration = {
                **settings["base_configuration"],
                "max_total_tokens": device,
                "hicache_ratio": host,
                "page_size": page,
                "hicache_write_policy": write,
                "hicache_storage_prefetch_policy": policy,
                "hicache_storage_backend_extra_config": json.dumps(extra),
            }
            same = {**configuration, "hicache_storage_backend_extra_config": extra} == reference
            result.append(
                {
                    "name": settings["policy"]["baseline"] if same else f"candidate_{len(result):03d}",
                    "configuration": configuration,
                }
            )
    return result


class SearchExample:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.settings = read_json(args.settings)
        policy = SearchPolicy.parse({"prediction": "prediction.json", **self.settings["policy"]})
        if policy.baseline in {".", ".."} or Path(policy.baseline).name != policy.baseline:
            raise ValueError("The example baseline name must be a single path component")
        self.output = args.output.resolve()
        self.targets = candidates(self.settings)
        if not any(target["name"] == policy.baseline for target in self.targets):
            raise ValueError("The example space must include its declared base configuration")
        self.capture = dict(self.settings["capture"])
        if args.devices is not None:
            self.capture["devices"] = args.devices
        self.repetitions = self.settings["normal_repetitions"]
        if type(self.repetitions) is not int or self.repetitions < 1:
            raise ValueError("Normal repetitions must be a positive integer")

    def prepare_request(self) -> Path:
        directory = self.output / "requests"
        directory.mkdir(parents=True, exist_ok=True)
        prediction = directory / "prediction.json"
        raw = {
            "deployment": {**self.settings["deployment"], "model": str(self.args.model.absolute())},
            "base": {"manifest": str(self.args.base_manifest.resolve())}
            if self.args.base_manifest
            else {"configuration": self.settings["base_configuration"]},
            "workload": str(self.args.workload.resolve()),
            "targets": self.targets,
            "performance": {
                "database": str(
                    self.args.database.resolve() if self.args.database else self.output / "performance.sqlite"
                ),
                "sampling_budget": self.settings["sampling_budget"],
            },
            "execution": {
                "output": str(self.output / "prediction_run"),
                "kernel_cache_initial_state": "empty",
                "memory_limit_gib": self.settings["prediction_memory_limit_gib"],
                "capture": {**self.capture, "budget": self.settings["source_budget"]},
            },
        }
        write_json(prediction, raw)
        path = directory / "search.json"
        write_json(path, {"prediction": str(prediction), **self.settings["policy"]})
        return path

    def recover(self) -> None:
        request = PredictionRequest.load(self.output / "requests/prediction.json")
        for directory, budget in (("base", "source_budget"), ("performance/sampling", "sampling_budget")):
            settings = replace(request.execution.capture, budget=self.settings[budget])
            DeviceTask(settings, request.execution.output / directory).recover()
        for target in self.targets:
            for repetition in range(self.repetitions):
                path = self.output / "requests" / f"normal_{target['name']}_{repetition}.json"
                if path.exists():
                    normal = PredictionRequest.load(path)
                    settings = replace(normal.execution.capture, budget=self.settings["normal_budget"])
                    DeviceTask(settings, normal.execution.output / "normal").recover()

    def confirm(self) -> None:
        result = read_json(self.output / "prediction_run/search/report.json")
        if result["status"] != "completed":
            raise ValueError("Finish prediction coverage before selecting normal confirmations")
        names = confirmation_names(result)
        configurations = {target["name"]: target["configuration"] for target in self.targets}
        plans = {plan["name"]: plan for plan in result["resource_plans"]}
        candidates_by_name = {row["name"]: row for row in result["candidates"]}
        raw = read_json(self.output / "requests/prediction.json")
        entries = []
        for name in names:
            entry = {"name": name, "observations": [], "errors": []}
            plan = plans[candidates_by_name[name]["resource_plan"]]
            for repetition in range(self.repetitions):
                output = self.output / "normal" / name / str(repetition + 1)
                request = {
                    **raw,
                    "base": {"configuration": configurations[name]},
                    "targets": [{"name": name, "configuration": {}}],
                    "execution": {
                        **raw["execution"],
                        "output": str(output),
                        "capture": {
                            **self.capture,
                            "memory_limit_gib": plan["host_gib"],
                            "budget": self.settings["normal_budget"],
                        },
                    },
                }
                path = self.output / "requests" / f"normal_{name}_{repetition}.json"
                write_json(path, request)

                measured = InferenceCapture(PredictionRequest.load(path), profiling=False).run(reuse=True)

                if measured["status"] in {"completed", "reused"}:
                    entry["observations"].append(measured["observation"])
                else:
                    entry["errors"].append(measured)
                write_json(self.output / "confirmation.json", {"entries": entries + [entry]})
            entries.append(entry)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--workload", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--devices", type=int, nargs="+")
    parser.add_argument("--database", type=Path)
    parser.add_argument("--base-manifest", type=Path)
    parser.add_argument("--settings", type=Path, default=EXAMPLE / "space.json")
    parser.add_argument("--phase", choices=("plan", "predict", "confirm", "report", "all"), default="all")
    args = parser.parse_args()
    example = SearchExample(args)
    if args.phase == "plan":
        print(
            json.dumps(
                {
                    "candidates": len(example.targets),
                    "normal_roles": ["baseline", "fastest", "selected"],
                    "normal_repetitions": example.repetitions,
                    "qualification": "Declaration only; resource and cost coverage are not yet qualified.",
                },
                indent=2,
            )
        )
        return

    example.output.mkdir(parents=True, exist_ok=True)
    with (example.output / "search.lock").open("a") as lease:
        fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
        request = example.prepare_request()
        example.recover()

        if args.phase in {"all", "predict"}:
            result = search(request)
            print(
                json.dumps(
                    {key: result.get(key) for key in ["status", "expected", "ranked", "selected", "fastest", "report"]}
                ),
                flush=True,
            )
            if result["status"] != "completed":
                raise SystemExit(1)

        if args.phase in {"all", "confirm"}:
            example.confirm()

        if args.phase in {"all", "report"}:
            result = write_report(example.output, example.targets, example.repetitions)
            if not result["confirmation_complete"]:
                raise SystemExit(1)


if __name__ == "__main__":
    main()
