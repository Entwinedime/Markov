"""Prepare assets once and predict a configuration/workload set."""

import fcntl
import hashlib
import json
import math
import shutil
import subprocess
import tempfile
import time
from datetime import datetime, timezone
from itertools import product
from pathlib import Path
from types import SimpleNamespace

from markov_adapters.artifacts import write_json
from markov_workflow.acquisition import SourceAcquisition
from markov_workflow.adapters.sglang_planning import SourcePlanning
from markov_workflow.cost_acquisition import CostAcquisition
from markov_workflow.exploration_report import ExplorationReport
from markov_workflow.latency import PredictedLatency
from markov_workflow.limits import AllowanceExhausted, OperationLimit
from markov_workflow.planning import PlanningProcess
from markov_workflow.prediction import PredictionProcess
from markov_workflow.production import CaptureProduction
from markov_workflow.source_productions import SourceProductions


SPENDING = ("native_charged_ns", "native_attempts", "postprocessing_ns")


def read(path):
    return json.loads(path.read_text())


class ExplorationAsset:
    def __init__(self, production, costs):
        self.production, self.costs = production.resolve(strict=True), costs.resolve(strict=True)
        self.sources = SourceProductions(self.production)

    def document(self):
        return {"production": str(self.production), "costs": str(self.costs)}


class ExplorationTarget:
    def __init__(self, root, index, model, configuration, workload):
        self.task = root
        self.root = root / "targets" / str(index)
        self.input = {"model": model, "instances": configuration["instances"], "workload": workload}
        self.identity = {"configuration": configuration["id"], "workload": workload["id"]}

    def predict(self, arguments, limit, asset, stage):
        input_path = self.root / "input.json"
        if not input_path.is_file():
            write_json(input_path, self.input, indent=2, replace=False)

        directory = self.root / stage
        outcome = directory / "outcome.json"
        if outcome.is_file():
            return read(outcome)
        directory.mkdir(parents=True, exist_ok=True)
        attempts = sorted((path for path in directory.iterdir() if path.is_dir()), key=lambda path: int(path.name))
        output = attempts[-1] if attempts else directory / "0"

        # A stopped coordinator can leave two named processes or a completed publication.
        if attempts and not (output / "result.json").is_file():
            PredictionProcess.recover(output)
        result, evidence = self.evidence(output)
        elapsed = None
        if result is None:
            if attempts:
                output = directory / str(int(output.name) + 1)
            start = time.monotonic_ns()
            try:
                PredictionProcess(SimpleNamespace(
                    **arguments, input=input_path, output=output, production=asset.production,
                    costs=asset.costs, execution_inputs=None,
                ), limit).run()
            except AllowanceExhausted:
                raise
            except (ValueError, RuntimeError, subprocess.SubprocessError, TimeoutError) as failure:
                result, evidence = self.evidence(output)
                if result is None:
                    timed_out = isinstance(failure, (subprocess.TimeoutExpired, TimeoutError))
                    result = {"status": "prediction_timeout" if timed_out else "prediction_failed", "message": str(failure)}
            else:
                result, evidence = self.evidence(output)
            elapsed = time.monotonic_ns() - start

        row = {**self.identity, **result, "asset": asset.document(), "asset_use": stage,
               "directory": str(output.relative_to(self.task)), "elapsed_ns": elapsed}
        if evidence is not None:
            row["evidence"] = str(evidence.relative_to(self.task))
        write_json(outcome, row, indent=2, replace=False)
        return row

    def evidence(self, output):
        result = output / "result.json"
        if result.is_file():
            metrics = PredictedLatency(read(result), self.input["workload"]["requests"]).report()
            requests = metrics.pop("requests")
            write_json(output / "request-latency.json", requests)
            return {"status": "predicted", **metrics}, result
        for status, name in (("template_gap", "template-gap.json"), ("cost_gap", "cost-gap.json")):
            path = output / name
            if path.is_file():
                return {"status": status}, path
        return None, None


class ExplorationTask:
    def __init__(self, args):
        self.args = args
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
        self.root = (args.output or Path("data") / f"{stamp}_exploration").resolve()
        self.binary = args.binary.resolve(strict=True)
        self.runtime = {}
        for role in ("source_image", "modeling_image", "control_image"):
            self.runtime[role] = subprocess.check_output(
                ["docker", "image", "inspect", getattr(args, role), "--format", "{{.Id}}"], text=True,
            ).strip()
        self.source = read(args.input)
        model = Path(self.source["model"]["path"]).resolve(strict=True)
        self.source["model"] = {**self.source["model"], "path": str(model)}
        planner = SourcePlanning(self.source)
        self.configurations, self.workloads = planner.configurations, planner.workloads
        self.reuse = args.reuse.resolve(strict=True) if args.reuse else None
        if self.root.is_relative_to(model) or (self.reuse and self.root.is_relative_to(self.reuse)):
            raise ValueError("exploration must own an output outside the model and prior task")

        seconds = (args.timeout, args.native_budget_seconds, args.postprocessing_budget_seconds)
        if any(not math.isfinite(value) or value < 0 for value in seconds) or args.timeout <= 0:
            raise ValueError("exploration needs a positive timeout and nonnegative finite time budgets")
        if args.storage_bytes <= 0 or args.attempt_limit < 0:
            raise ValueError("exploration needs nonnegative attempts and positive storage")
        self.budget = dict(zip(SPENDING, (int(args.native_budget_seconds * 1e9), args.attempt_limit,
                                        int(args.postprocessing_budget_seconds * 1e9)), strict=True))
        self.limit = OperationLimit(self.root, args.storage_bytes)
        self.environment = {
            "framework": self.source["framework"], "model": self.source["model"], "deployment": read(args.deployment),
            "model_config": hashlib.sha256((model / "config.json").read_bytes()).hexdigest(),
            "binary": hashlib.sha256(self.binary.read_bytes()).hexdigest(), "images": self.runtime,
        }
        self.inputs = {"source": self.source, "environment": self.environment, "details": args.details,
                       "reuse": str(self.reuse) if self.reuse else None}
        self.training = self.root / "collection"
        self.cost_task = self.root / "assets"
        self.reuse_status = None

    def prepare(self):
        directory = self.root / "inputs"
        manifest = directory / "task.json"
        executable = directory / "markov"
        if manifest.is_file():
            if self.inputs != read(manifest) or hashlib.sha256(executable.read_bytes()).hexdigest() != self.environment["binary"]:
                raise ValueError("exploration input, runtime or executable changed; use a new task")
        else:
            if any(path.name != ".lock" for path in self.root.iterdir()):
                raise ValueError("new exploration needs an empty output directory")
            directory.mkdir()
            shutil.copyfile(self.binary, executable)
            executable.chmod(0o500)
            write_json(directory / "deployment.json", self.environment["deployment"], indent=2)
            write_json(manifest, self.inputs, indent=2, replace=False)
        self.binary = executable

    def spending(self):
        records = [read(path) for directory in (self.training, self.cost_task)
                   if (path := directory / "status.json").is_file()]
        return {kind: sum(record[kind] for record in records) for kind in SPENDING}

    def allowance(self, own):
        path = own / "status.json"
        charged = read(path) if path.exists() else dict.fromkeys(SPENDING, 0)
        used = self.spending()
        available = {key: charged[key] + max(0, self.budget[key] - used[key]) for key in SPENDING}
        return {
            "native_budget_seconds": available["native_charged_ns"] / 1e9,
            "attempt_limit": available["native_attempts"], "postprocessing_budget_seconds": available["postprocessing_ns"] / 1e9,
            "storage_bytes": self.args.storage_bytes, "timeout": self.args.timeout, "binary": self.binary,
            "source_image": self.runtime["source_image"], "modeling_image": self.runtime["modeling_image"],
        }

    def previous(self):
        if self.reuse is None:
            return []
        prior = read(self.reuse / "inputs/task.json")
        if prior["environment"] != self.environment:
            self.reuse_status = "different_model_deployment_or_runtime"
            return []
        prior_result = read(self.reuse / "result.json")
        self.reuse_status = "considered"
        return [ExplorationAsset(Path(row["production"]), Path(row["costs"])) for row in prior_result["assets"]]

    def plan(self, pending):
        selected = {field: {target.identity[singular] for target in pending}
                    for field, singular in (("configurations", "configuration"), ("workloads", "workload"))}
        source = {**self.source, **{field: [row for row in self.source[field] if row["id"] in names]
                                    for field, names in selected.items()}}
        directory = self.root / "plan"
        if not (directory / "plan.json").is_file():
            training = self.root / "inputs/training.json"
            write_json(training, source, indent=2)
            with tempfile.TemporaryDirectory(prefix=".planning-", dir=self.root) as temporary:
                staged = Path(temporary) / "plan"
                PlanningProcess(SimpleNamespace(input=training, output=staged), prewarm=True).run()
                staged.rename(directory)
        return directory

    def prepare_assets(self, pending, warm, previous):
        plan = self.plan(pending)
        costs = self.cost_task / "costs.json"
        if not costs.is_file():
            allowance = self.allowance(self.training)
            if not (self.training / "result.json").is_file() or not (self.training / "status.json").is_file():
                if min(allowance["native_budget_seconds"], allowance["postprocessing_budget_seconds"], allowance["attempt_limit"]) <= 0:
                    raise AllowanceExhausted("no allowance to prepare missing source assets")
                SourceAcquisition(SimpleNamespace(
                    **allowance, output=self.training, plan=plan, deployment=self.root / "inputs/deployment.json",
                    observations=None, keep_intermediates=True, resource_trace=self.args.details,
                ), self.limit).run(allow_incomplete=True)

            collection = SourceProductions(self.training)
            if not any(collection.select(target.input) is not None for target in pending):
                return None, collection
            allowance = self.allowance(self.cost_task)
            if allowance["postprocessing_budget_seconds"] <= 0:
                raise AllowanceExhausted("no allowance to prepare costs")
            samples = {Path(path) for asset in previous for path in read(asset.costs)["sources"]}
            gaps = [self.root / row["evidence"] for row in warm if row["status"] == "cost_gap"]
            CostAcquisition(SimpleNamespace(
                **allowance, output=self.cost_task, production=[self.training], samples=sorted(samples), demand=[], gap=gaps,
            ), self.limit).run()
        asset = ExplorationAsset(self.training, costs)

        # The original observations can regenerate source projections after the cost asset is closed.
        if not self.args.details:
            for production in asset.sources.paths.values():
                ranks = read(production / "result.json")["observed_roles"]
                CaptureProduction.trim(production, ranks, "templates")
        return asset, asset.sources

    def explore(self):
        previous = self.previous()
        runtime = {"binary": self.binary, "control_image": self.runtime["control_image"],
                   "modeling_image": self.runtime["modeling_image"], "timeout": self.args.timeout,
                   "execution_trace": self.args.details}
        targets = [ExplorationTarget(self.root, index, self.source["model"], configuration, workload)
                   for index, (configuration, workload) in enumerate(
                       product(self.configurations.values(), self.workloads.values()))]
        rows, pending, warm, used = {}, [], [], {}
        for target in targets:
            asset = next((item for item in previous if item.sources.select(target.input) is not None), None)
            if asset is None:
                pending.append(target)
                continue
            row = target.predict(runtime, self.limit, asset, "reused")
            used[asset.production] = asset
            rows[target] = row
            if row["status"] in {"cost_gap", "template_gap"}:
                pending.append(target)
                warm.append(row)

        if pending:
            try:
                asset, collection = self.prepare_assets(pending, warm, previous)
            except AllowanceExhausted as failure:
                for target in pending:
                    rows.setdefault(target, {**target.identity, "status": "budget_exhausted", "message": str(failure)})
            else:
                if asset:
                    used[asset.production] = asset
                for target in pending:
                    if asset and collection.select(target.input) is not None:
                        rows[target] = target.predict(runtime, self.limit, asset, "prepared")
                    else:
                        rows.setdefault(target, {**target.identity, **collection.failure(target.input)})

        assets = [asset.document() for asset in reversed(list(used.values()))]
        usage = {**self.spending(), "retained_bytes": self.limit.bytes_used()}
        return ExplorationReport([rows[target] for target in targets], assets, usage, self.reuse_status)

    def run(self):
        self.root.mkdir(parents=True, exist_ok=True)
        with (self.root / ".lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.prepare()
            result = self.root / "result.json"
            if not result.is_file():
                report = self.explore()
                (self.root / "summary.md").write_text(report.markdown())
                write_json(result, report.document(), indent=2, replace=False)
        print(self.root / "summary.md")
        return read(result)["status"] == "complete"
