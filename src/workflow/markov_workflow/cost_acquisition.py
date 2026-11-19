"""Publish one bounded set of costs using observed graphs and native host recipes."""

import fcntl
import filecmp
import json
import math
import shutil
import subprocess
import sys
import time
from contextlib import contextmanager
from types import SimpleNamespace
from uuid import uuid4

from markov_adapters.artifacts import write_json
from markov_workflow.adapters.sglang_host_samples import HostCostSamples
from markov_workflow.capture import CaptureProcess
from markov_workflow.containers import OwnedContainers
from markov_workflow.cost_requirements import CostRequirements
from markov_workflow.cost_samples import WorkCatalog
from markov_workflow.fit_costs import FittedCosts
from markov_workflow.limits import AllowanceExhausted, OperationLimit
from markov_workflow.source_productions import SourceProductions


def read(path):
    return json.loads(path.read_text())


class CostSource:
    def __init__(self, production, capture, job, output):
        self.production, self.capture, self.job, self.output = production, capture, job, output
        CaptureProcess.published(capture, job)
        self.deployment = read(capture / "execution.json")["inputs"]["deployment"]
        self.ranks = read(production / "result.json")["observed_roles"]
        self.recipes = {recipe.name: recipe for recipe in HostCostSamples.recipes(job)}
        self.host_points = set()
        self.cold = False


class CostAcquisition:
    def __init__(self, args, limit=None):
        self.args = args
        self.output = args.output.resolve()
        self.binary = args.binary.resolve(strict=True)
        if any(not math.isfinite(value) or value < 1e-9 for value in (args.timeout, args.postprocessing_budget_seconds)):
            raise ValueError("cost acquisition needs positive finite time allowances")
        if (not math.isfinite(args.native_budget_seconds) or args.native_budget_seconds < 0
                or args.attempt_limit < 0 or args.storage_bytes <= 0):
            raise ValueError("cost acquisition needs nonnegative native allowances and positive storage")
        self.native_ns = int(args.native_budget_seconds * 1e9)
        self.postprocessing_ns = int(args.postprocessing_budget_seconds * 1e9)
        self.limit = limit if limit is not None else OperationLimit(self.output, args.storage_bytes)
        self.images = {
            role: subprocess.check_output(["docker", "image", "inspect", image, "--format", "{{.Id}}"], text=True).strip()
            for role, image in (("source", args.source_image), ("modeling", args.modeling_image))
        }

        collections = list(dict.fromkeys(path.resolve(strict=True) for path in args.production))
        self.sources = []
        seen = set()
        for collection in collections:
            bundle = SourceProductions(collection)
            for identity, production in bundle.paths.items():
                capture = bundle.captures[identity].resolve(strict=True)
                production = production.resolve(strict=True)
                if any(root == self.output or root in self.output.parents for root in (collection, production, capture)):
                    raise ValueError("cost acquisition must own an output outside its training sources")
                if (production, capture) not in seen:
                    seen.add((production, capture))
                    directory = self.output / "sources" / str(len(self.sources))
                    self.sources.append(CostSource(production, capture, bundle.jobs[str(identity)], directory))

        self.samples = {path.resolve(strict=True): read(path) for path in args.samples}
        self.gaps = {path.resolve(strict=True): read(path) for path in args.gap}
        self.graphs = {graph.resolve(strict=True): identities.resolve(strict=True) for graph, identities in args.demand}
        self.inputs = {
            "collections": [str(path) for path in collections], "images": self.images,
            "samples": [str(path) for path in self.samples], "gaps": [str(path) for path in self.gaps],
            "demand": [[str(graph), str(identities)] for graph, identities in self.graphs.items()],
        }
        self.demand = CostRequirements()

    def save(self):
        write_json(self.output / "execution.json", self.state, indent=2)

    def prepare(self):
        executable = self.output / "markov"
        state_path = self.output / "execution.json"
        if state_path.exists():
            self.state = read(state_path)
            if self.state["inputs"] != self.inputs or not filecmp.cmp(self.binary, executable, shallow=False):
                raise ValueError("cost acquisition inputs or executable changed")
            if self.state["container"] is not None:
                OwnedContainers([self.state["container"]]).close()
                self.state["container"] = None
                self.save()
            return
        if any(path.name != ".lock" for path in self.output.iterdir()):
            raise ValueError("new cost acquisition requires an empty directory")
        shutil.copyfile(self.binary, executable)
        executable.chmod(0o500)
        self.state = {"inputs": self.inputs, "postprocessing_ns": 0, "container": None}
        self.save()

    @contextmanager
    def processing(self, container=None):
        spent = self.state["postprocessing_ns"]
        remaining = self.postprocessing_ns - spent
        if remaining <= 0:
            raise AllowanceExhausted("cost postprocessing allowance exhausted")

        # Reserve before launch so interruption cannot make unfinished work free.
        self.state.update(postprocessing_ns=self.postprocessing_ns, container=container)
        self.save()
        started = time.monotonic_ns()
        try:
            self.limit.check()
            yield remaining / 1e9
            self.limit.check()
            if time.monotonic_ns() - started > remaining:
                raise AllowanceExhausted("cost postprocessing allowance exhausted")
        finally:
            if container is not None:
                OwnedContainers([container]).close()
            self.state.update(postprocessing_ns=spent + time.monotonic_ns() - started, container=None)
            self.save()

    def command(self, output, command, graph=None):
        if output.is_file():
            return
        output.parent.mkdir(parents=True, exist_ok=True)
        name = f"markov-work-{uuid4().hex}" if graph is not None else None
        with self.processing(name) as allowance, output.with_suffix(".log").open("a") as log:
            if graph is not None:
                command = [
                    "docker", "run", "--rm", "--init", "--runtime", "runc", "--network", "none", "--name", name,
                    "--entrypoint", "/usr/bin/timeout",
                    "--mount", f"type=bind,src={graph},dst=/graph.json,readonly",
                    "--mount", f"type=bind,src={self.output},dst=/task",
                    self.images["modeling"], "--signal=KILL", f"{allowance}s", "/task/markov",
                    "work-identities", "/graph.json", f"/task/{output.relative_to(self.output)}",
                ]
            result = self.limit.run(command, log, allowance)
            if result:
                raise subprocess.CalledProcessError(result, command)

    def samples_from(self, output, action, **inputs):
        command = [sys.executable, "-m", "markov_workflow", action]
        for name, value in {**inputs, "output": output}.items():
            command.extend(("--" + name.replace("_", "-"), str(value)))
        self.command(output, command)
        sample = read(output)
        self.samples[output] = sample
        return sample

    def inspect(self, source):
        for rank in source.ranks:
            work = source.production / "work"
            graph = work / f"{rank}-graph.json"
            identities = source.output / f"{rank}-work.json"
            self.command(identities, None, graph)
            with self.processing():
                catalog = WorkCatalog(graph, identities)
                for point in self.demand.graph(str(graph), catalog):
                    definition = self.demand.definitions[point.definition]
                    interval = definition["work"][point.interval]
                    if HostCostSamples.supports(interval, definition["resources"][interval["resource"]]):
                        source.host_points.add(point)

            # The same detailed observation must not contribute device durations twice.
            observation = {"production": str(source.production), "rank": rank}
            if not any(sample.get("observation") == observation for sample in self.samples.values()):
                self.samples_from(source.output / f"{rank}-device.json", "cost-samples",
                                  production=source.production, rank=rank, work_identities=identities)

    @staticmethod
    def attempts(path):
        state = path / "execution.json"
        return read(state)["attempts"] if state.exists() else []

    def usage(self):
        attempts = [attempt for source in self.sources for recipe in source.recipes
                    for attempt in self.attempts(source.output / recipe)]
        return sum(attempt["charged_ns"] for attempt in attempts), len(attempts)

    def spending(self):
        spent, attempts = self.usage()
        return {"native_charged_ns": spent, "native_attempts": attempts,
                "postprocessing_ns": self.state["postprocessing_ns"]}

    def measure(self, source, recipe, allowance_ns):
        output = source.output / recipe
        job_path, deployment_path = source.output / f"{recipe}-input.json", source.output / "deployment.json"
        write_json(job_path, source.recipes[recipe].job)
        write_json(deployment_path, source.deployment)
        CaptureProcess(SimpleNamespace(
            input=job_path, deployment=deployment_path, output=output, timeout=self.args.timeout,
            budget_seconds=allowance_ns / 1e9, attempt_limit=1, source_image=self.images["source"], resource_trace=False,
        ), self.limit, host_timing=True).run()
        for rank in source.ranks:
            work = source.production / "work"
            sample = self.samples_from(
                source.output / f"{recipe}-{rank}-host.json", "host-cost-samples", detailed=source.capture, host=output,
                facts=work / f"{rank}-facts.json", trace=work / f"{rank}-sdk/trace.json",
                graph=work / f"{rank}-graph.json", work_identities=source.output / f"{rank}-work.json",
            )
            source.cold |= bool(sample["qualification"]["excluded_cold_scopes"])

    def fit(self):
        with self.processing():
            return FittedCosts.qualify(self.samples, self.demand)

    def acquire(self):
        for source in self.sources:
            self.inspect(source)
        with self.processing():
            for graph, identities in self.graphs.items():
                if str(graph) not in self.demand.targets:
                    self.demand.graph(str(graph), WorkCatalog(graph, identities))
            for path, gap in self.gaps.items():
                self.demand.gap(str(path), gap)

        candidates = {(source, recipe): self.attempts(source.output / recipe)
                      for source in self.sources for recipe in source.recipes}
        fitted, reused, counts, missing = self.fit()
        evidence = []
        deferred = set()
        while candidates and missing:

            # A single native session can cover work required by multiple targets and ranks.
            requested = {source: len(source.host_points & missing.keys()) for source in self.sources}
            applicable = [(source, recipe) for source, recipe in candidates if requested[source]
                          and (source.cold or not source.recipes[recipe].requires_cold)]
            if not applicable:
                break
            source, recipe = max(applicable, key=lambda key: (bool(candidates[key]), requested[key[0]]))
            output = source.output / recipe
            previous = candidates[source, recipe]
            spent, attempts = self.usage()
            allowance = max(0, self.native_ns - spent)
            if not previous and (not allowance or attempts >= self.args.attempt_limit):
                deferred.update(*(candidate.host_points for candidate, _ in applicable))
                break

            del candidates[source, recipe]
            row = {"capture": str(output.relative_to(self.output)), "candidate_points": requested[source]}
            evidence.append(row)
            if previous and previous[-1]["status"] == "failed":
                row["status"] = "capture_failed"
                continue
            try:
                self.measure(source, recipe, allowance + sum(attempt["charged_ns"] for attempt in previous))
            except (subprocess.CalledProcessError, subprocess.TimeoutExpired, RuntimeError):
                if not self.attempts(output) or self.attempts(output)[-1]["status"] != "failed":
                    raise
                self.limit.check()
                row["status"] = "capture_failed"
                continue
            before = missing
            fitted, reused, counts, missing = self.fit()
            row.update(status="measured", additional_points=len(before.keys() - missing.keys()))

        observed = set().union(*(source.host_points for source in self.sources))
        reasons = {
            point: ("native_budget_exhausted" if point in deferred else
                    "no_qualified_measurement" if point in observed else "no_observed_recipe")
            for point in missing
        }
        return {
            "domains": fitted.document(), "sources": [str(path) for path in self.samples], "reused": reused,
            "demand": self.demand.report(counts, missing, reasons), "recipes": evidence,
            **self.spending(), "retained_bytes": self.limit.bytes_used(),
            "status": "covered" if not missing else "native_budget_exhausted" if deferred else "uncovered",
        }

    def run(self):
        self.output.mkdir(parents=True, exist_ok=True)
        with (self.output / ".lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.prepare()
            result = self.output / "costs.json"
            try:
                if not result.is_file():
                    write_json(result, self.acquire(), replace=False)
            finally:
                write_json(self.output / "status.json", self.spending(), indent=2)
        print(f"Created {result}")
