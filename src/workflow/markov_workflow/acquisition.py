"""Execute a finite source plan using the existing capture and production owners."""

import fcntl
import filecmp
import json
import math
import shutil
import subprocess
from pathlib import Path
from types import SimpleNamespace

from markov_adapters.sglang.configuration import CaptureInput
from markov_collection.artifacts import write_json
from markov_workflow.adapters.ascend import AscendDeployment
from markov_workflow.adapters.sglang import SourceDomain
from markov_workflow.capture import CaptureProcess
from markov_workflow.limits import OperationLimit
from markov_workflow.production import TemplateProduction


class SourceAcquisition:
    def __init__(self, arguments):
        self.arguments = arguments
        self.output = arguments.output.resolve()
        plan_root = arguments.plan.resolve(strict=True)
        plan = json.loads((plan_root / "plan.json").read_text())
        deployment = json.loads(arguments.deployment.read_text())
        self.jobs, self.domains = {}, {}
        for domain in plan["domains"]:
            identity = domain["id"]
            if type(identity) is not int or identity < 0 or identity in self.jobs:
                raise ValueError("source plan needs distinct nonnegative domain IDs")
            if domain["source"]["unrepresented_conditions"]:
                raise ValueError(f"domain {identity} has unresolved input conditions: {domain['source']['unrepresented_conditions']}")
            path = (plan_root / domain["source"]["input"]).resolve(strict=True)
            if not path.is_relative_to(plan_root):
                raise ValueError("planned capture input lies outside its plan directory")
            job = json.loads(path.read_text())
            serving = CaptureInput(job)
            if SourceDomain.requested(serving) != domain["structure"]:
                raise ValueError("planned capture input differs from its source domain")
            workers = sum(instance.configuration.tp * instance.configuration.pp for instance in serving.instances)
            placement = AscendDeployment(deployment, workers, len(serving.instances) > 1)
            model = Path(serving.model["path"]).resolve(strict=True)
            if self.output == model or model in self.output.parents:
                raise ValueError("source collection must not write inside a model directory")
            self.jobs[identity], self.domains[identity] = job, domain["structure"]
        if not self.jobs:
            raise ValueError("source acquisition needs a nonempty plan")
        for value in (arguments.native_budget_seconds, arguments.postprocessing_budget_seconds, arguments.timeout):
            if not math.isfinite(value) or value < 1e-9:
                raise ValueError("collection time allowances must be finite and at least one nanosecond")
        if arguments.attempt_limit <= 0 or arguments.storage_bytes <= 0:
            raise ValueError("collection attempt and storage allowances must be positive")
        self.binary = arguments.binary.resolve(strict=True)
        if not self.binary.is_file():
            raise ValueError("source collection requires a compiled executable")

        # Installation mounts are common to every job; check them before the first native launch.
        placement.options()
        self.native_ns = int(arguments.native_budget_seconds * 1_000_000_000)
        self.production_ns = int(arguments.postprocessing_budget_seconds * 1_000_000_000)
        self.limit = OperationLimit(self.output, arguments.storage_bytes)
        images = {role: subprocess.check_output(["docker", "image", "inspect", image, "--format", "{{.Id}}"], text=True).strip()
                  for role, image in (("source", arguments.source_image), ("modeling", arguments.modeling_image))}
        self.inputs = {"plan": plan, "jobs": {str(key): value for key, value in self.jobs.items()},
                       "deployment": deployment, "images": images, "resource_trace": arguments.resource_trace,
                       "keep_intermediates": arguments.keep_intermediates}

    def usage(self):
        native, production, attempts = {}, {}, {}
        for identity in self.jobs:
            path = self.output / "sources" / str(identity) / "execution.json"
            records = json.loads(path.read_text())["attempts"] if path.exists() else []
            native[identity] = sum(record["charged_ns"] for record in records)
            attempts[identity] = len(records)
            path = self.output / "assets" / str(identity) / "production.json"
            production[identity] = json.loads(path.read_text())["spent_ns"] if path.exists() else 0
        return native, production, attempts

    def status(self, error):
        native, production, attempts = self.usage()
        write_json(self.output / "status.json", {
            "native_charged_ns": sum(native.values()), "postprocessing_ns": sum(production.values()),
            "native_attempts": sum(attempts.values()), "retained_bytes": self.limit.bytes_used(),
            "limits": {"native_ns": self.native_ns, "postprocessing_ns": self.production_ns,
                       "native_attempts": self.arguments.attempt_limit, "storage_bytes": self.arguments.storage_bytes},
            "error": error,
        }, indent=2)

    def prepare(self):
        path = self.output / "input.json"
        executable = self.output / "inputs/markov"
        if path.exists():
            if json.loads(path.read_text()) != self.inputs or not filecmp.cmp(self.binary, executable, shallow=False):
                raise ValueError("collection inputs, program, images or retention policy changed; use a new directory")
        else:
            if any(path.name != ".lock" for path in self.output.iterdir()):
                raise ValueError("new source collection requires an empty directory")
            executable.parent.mkdir()
            shutil.copyfile(self.binary, executable)
            executable.chmod(0o500)
            write_json(path, self.inputs, indent=2, replace=False)

        # Every domain uses this task's program, even if the external build changes during native capture.
        self.binary = executable

    def acquire(self, identity):
        native, production, attempts = self.usage()
        native_allowance = native[identity] + max(0, self.native_ns - sum(native.values()))
        attempt_allowance = attempts[identity] + max(0, self.arguments.attempt_limit - sum(attempts.values()))
        production_allowance = production[identity] + max(0, self.production_ns - sum(production.values()))
        if min(native_allowance, production_allowance, attempt_allowance) <= 0:
            raise RuntimeError("source collection cumulative allowance exhausted")
        job_path, deployment_path = self.output / ".capture-input.json", self.output / ".deployment.json"
        write_json(job_path, self.jobs[identity])
        write_json(deployment_path, self.inputs["deployment"])
        source = self.output / "sources" / str(identity)
        assets = self.output / "assets" / str(identity)

        # Children recover before storage admission; completed work never reserves another operation.
        CaptureProcess(SimpleNamespace(
            input=job_path, deployment=deployment_path, output=source, timeout=self.arguments.timeout,
            budget_seconds=native_allowance / 1_000_000_000, attempt_limit=attempt_allowance,
            source_image=self.inputs["images"]["source"], resource_trace=self.arguments.resource_trace,
        ), self.limit).run()
        publication = json.loads((source / "result.json").read_text())
        TemplateProduction(SimpleNamespace(
            capture=source / publication["source"], output=assets, binary=self.binary,
            budget_seconds=production_allowance / 1_000_000_000, source_image=self.inputs["images"]["source"],
            modeling_image=self.inputs["images"]["modeling"], keep_intermediates=self.arguments.keep_intermediates,
        ), self.limit).run()
        publication = json.loads((assets / "result.json").read_text())
        libraries = [json.loads((assets / path).read_text()) for path in publication["templates"]]
        if SourceDomain.observed(libraries) != self.domains[identity]:
            raise ValueError(f"domain {identity} compiled a different native implementation")

    def run(self):
        self.output.mkdir(parents=True, exist_ok=True)
        with (self.output / ".lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.prepare()
            error = None
            try:
                for identity in self.jobs:
                    self.acquire(identity)
                self.limit.check()
                write_json(self.output / "result.json", {
                    "productions": [f"assets/{identity}" for identity in self.jobs],
                    "qualification": "compiled_initial_sources",
                    "limits": self.inputs["plan"]["limits"],
                }, indent=2)
            except BaseException as failure:
                error = f"{type(failure).__name__}: {failure}"
                raise
            finally:
                (self.output / ".capture-input.json").unlink(missing_ok=True)
                (self.output / ".deployment.json").unlink(missing_ok=True)
                self.status(error)
