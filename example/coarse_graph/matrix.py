"""Collect sources and independent targets, then compare a frozen execution-structure matrix."""

import argparse
import fcntl
import json
import re
import time
from contextlib import ExitStack
from pathlib import Path

from markov_adapters.artifacts import write_json
from markov_workflow.capture import CaptureProcess
from markov_workflow.containers import OwnedContainers
from markov_workflow.limits import OperationLimit

from run import SourceProductions, WorkflowCommands


class MatrixRun:
    def __init__(self, root, matrix):
        self.root = root.resolve(strict=True)
        self.repository = Path(__file__).resolve().parents[2]
        self.matrix = json.loads(matrix.read_text())
        environment = json.loads((matrix.parent / "environment.json").read_text())
        self.images, self.budget = environment["images"], environment["validation_budget"]
        self.binary = (matrix.parent / "markov").resolve(strict=True)
        self.name = self.matrix["id"]
        domains = [domain["id"] for domain in self.matrix["domains"]]
        cases = [case["id"] for domain in self.matrix["domains"] for case in domain["targets"]]
        if (not domains or not cases or len(set(domains)) != len(domains) or len(set(cases)) != len(cases)
                or any(re.fullmatch("[a-z0-9][a-z0-9-]*", name) is None for name in [self.name, *domains, *cases])):
            raise ValueError("matrix needs unique domain/case identifiers containing lowercase letters, digits and hyphens")
        self.commands = WorkflowCommands(self.repository, self.images)
        self.logs = self.root / "logs/independent"
        self.steps = self.logs / self.name
        self.steps.mkdir(parents=True, exist_ok=True)
        self.results = self.root / "results" / self.name
        self.results.mkdir(parents=True, exist_ok=True)
        self.storage = OperationLimit(self.root, self.budget["retained_bytes"])

    def step(self, name, command, charges, timeout, owner=None):
        container = f"markov-matrix-{self.name}-{name.replace('/', '--')}"
        command = command[:2] + ["--name", container] + command[2:]
        path = self.steps / f"{name}.step.json"
        if path.exists():
            record = json.loads(path.read_text())
            if record["command"] != command:
                raise ValueError(f"frozen step command changed: {name}")
            if record["exit_code"] is None:
                raise RuntimeError(f"inspect the recorded process and containers before resuming: {path}")
            return record["exit_code"]

        # All investigation stages share one reservation owner; terminal failures never trigger retries.
        with (self.logs / ".budget.lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            previous = [json.loads(p.read_text()) for p in self.logs.rglob("*.step.json")]
            unavailable = [category for category, amount in charges.items()
                           if sum(row["charges"].get(category, 0) for row in previous) + amount > self.budget[category]]
            self.storage.check()
            path.parent.mkdir(parents=True, exist_ok=True)
            record = {"command": command, "charges": charges, "charge_basis": "reserved", "exit_code": None}
            if unavailable:
                record.update(charges={}, exit_code=125, error=f"allowance not available: {unavailable}")
            write_json(path, record, indent=2, replace=False)
        if unavailable:
            return 125

        print(f"{name}: starting", flush=True)
        started = time.monotonic()
        try:
            with path.with_suffix(".log").open("x") as log:
                code = self.storage.run(command, log, timeout)
        except BaseException as error:

            # A dead coordinator does not prove its native child stopped; retain the unresolved reservation.
            record["error"] = f"{type(error).__name__}: {error}"
            write_json(path, record, indent=2)
            OwnedContainers([container]).close()
            if isinstance(error, Exception):
                raise RuntimeError(f"operation has no confirmed terminal result: {path}") from error
            raise

        elapsed = time.monotonic() - started
        record.update(exit_code=code, elapsed_seconds=elapsed)
        try:
            if owner is None:
                record.update(charges={"processing_seconds": elapsed}, charge_basis="elapsed")
            elif owner.exists():
                state = json.loads(owner.read_text())
                if owner.name == "execution.json":
                    attempts = state["attempts"]
                    record["charges"] = {"native_seconds": max(elapsed, sum(row["charged_ns"] for row in attempts) / 1e9),
                                         "native_attempts": len(attempts)}
                else:
                    native = state["native_charged_ns"] / 1e9
                    record["charges"] = {"native_seconds": native, "native_attempts": state["native_attempts"],
                                         "processing_seconds": max(state["postprocessing_ns"] / 1e9, elapsed - native)}
                record["charge_basis"] = "owner"
        finally:

            # A malformed owner report cannot erase an already observed process exit or release its reservation.
            write_json(path, record, indent=2)
        print(f"{name}: exit {code}", flush=True)
        return code

    def process(self, name, command):
        allowance = self.budget["operation_seconds"]

        # Reserve coordinator shutdown too; the child's declared work allowance remains unchanged.
        return self.step(name, command, {"processing_seconds": allowance + 60}, allowance + 60)

    def evaluate(self, name, script, *arguments):
        command = [
            "docker", "run", "--rm", "--init", "--runtime", "runc", "--network", "none",
            "--mount", f"type=bind,src={self.repository},dst={self.repository}",
            "--env", f"PYTHONPATH={self.repository}/src/collection:{self.repository}/src/adapters",
            "--workdir", str(self.repository), "--entrypoint", "timeout", self.images["control"],
            "--signal=KILL", str(self.budget["operation_seconds"]), "python3",
            str(self.repository / "example/coarse_graph" / script), *map(str, arguments),
        ]
        return self.process(name, command)

    @staticmethod
    def mapped(report):
        return (bool(report["ranks"]) and report["native_case"]["status"] in {"consistent", "incomplete"}
                and all(row["mapping"] == "consistent" and row["timing"]["status"] == "compatible" for row in report["ranks"]))

    def sources(self, domain):
        name = domain["id"]
        plan = self.root / "assets" / self.name / name / "plan"
        code = self.process(f"{name}/plan", self.commands.command(
            "plan-sources", "--input", self.root / domain["exploration"], "--output", plan,
        ))
        result = {"id": name, "plan_exit": code, "plan": str(plan)}
        if code:
            return result | {"status": "planning-failed"}

        specification = json.loads((plan / "plan.json").read_text())
        jobs = specification["native_jobs"]
        native, processing = jobs * self.budget["task_seconds"], jobs * self.budget["operation_seconds"]
        collection = self.root / "sources" / self.name / name
        model = json.loads((self.root / domain["exploration"]).read_text())["model"]["path"]
        command = self.commands.command(
            "acquire-sources", "--plan", plan, "--deployment", self.root / domain["deployment"], "--output", collection,
            "--binary", self.binary, "--source-image", self.images["source"], "--modeling-image", self.images["modeling"],
            "--timeout", self.budget["task_seconds"], "--native-budget-seconds", native,
            "--postprocessing-budget-seconds", processing, "--attempt-limit", jobs,
            "--storage-bytes", self.budget["retained_bytes"], "--resource-trace", "--keep-intermediates", model=model,
        )
        code = self.step(f"{name}/sources", command,
                         {"native_seconds": native + 60, "processing_seconds": processing + 60, "native_attempts": jobs},
                         native + processing + 60, collection / "status.json")
        result.update(acquisition_exit=code, collection=str(collection))
        if code:
            return result | {"status": "source-acquisition-failed"}

        published = SourceProductions(collection, plan)
        checks = []
        for job in published.plan["domains"]:
            identity = job["id"]
            observation = CaptureProcess.published(published.captures[identity], json.loads((plan / job["source"]["input"]).read_text()))
            output = self.results / "sources" / name / str(identity)
            self.evaluate(f"{name}/source-evidence-{identity}", "source_evidence.py",
                          "--production", published.paths[identity], "--capture", observation, "--output", output)
            path = output / "source-evidence.json"
            report = json.loads(path.read_text()) if path.exists() else None
            checks.append({"job": identity, "report": str(path), "mapped": report is not None and self.mapped(report),
                           "native_group_status": None if report is None else report["native_case"]["status"]})
        return result | {"status": "ready" if all(row["mapped"] for row in checks) else "source-evidence-unqualified", "jobs": checks}

    def target(self, domain, case):
        name = case["id"]
        directory = self.root / "targets" / self.name / name
        task = self.root / case["capture_input"]
        output = directory / "native"
        command = self.commands.command(
            "capture", "--input", task, "--deployment", self.root / domain["deployment"], "--output", output,
            "--source-image", self.images["source"], "--timeout", self.budget["task_seconds"],
            "--budget-seconds", self.budget["task_seconds"], "--attempt-limit", 1, "--resource-trace",
            model=json.loads(task.read_text())["model"]["path"],
        )
        code = self.step(f"{name}/capture", command,
                         {"native_seconds": self.budget["task_seconds"] + 60, "native_attempts": 1},
                         self.budget["task_seconds"] + 60, output / "execution.json")
        result = {"id": name, "capture_exit": code}
        if code:
            return result | {"status": "native-capture-failed"}

        native = CaptureProcess.published(output, json.loads(task.read_text()))
        production = directory / "structure"
        code = self.process(f"{name}/produce", self.commands.command(
            "produce", "--capture", native, "--product", "graphs", "--output", production, "--binary", self.binary,
            "--source-image", self.images["source"], "--modeling-image", self.images["modeling"],
            "--budget-seconds", self.budget["operation_seconds"], "--keep-intermediates",
        ))
        result.update(native=str(native), production=str(production), production_exit=code)
        if code:
            return result | {"status": "target-production-failed"}
        output = self.results / name / "source-evidence"
        result["evidence_exit"] = self.evaluate(f"{name}/source-evidence", "source_evidence.py",
                                               "--production", production, "--capture", native, "--output", output)
        path = output / "source-evidence.json"
        if not path.exists():
            return result | {"status": "source-evaluation-error"}
        report = json.loads(path.read_text())
        return result | {"status": "mapped" if self.mapped(report) else "target-evidence-unqualified",
                         "native_group_status": report["native_case"]["status"], "source_evidence": str(path)}

    def compare(self, domain, case, source, target):
        name = case["id"]
        result = {"id": name, "source_status": source["status"], "target_status": target["status"]}
        if target["status"] != "mapped":
            return result | {"status": "target-prerequisite-unqualified"}
        native, production = Path(target["native"]), Path(target["production"])
        output = self.results / name

        # Independent target facts supply only explicit time conditions, never source bodies or shape answers.
        self.evaluate(f"{name}/timing", "execution_inputs.py", "--capture", native, "--graphs", production / "work",
                      "--output", output / "timing")
        path = output / "timing/execution-inputs-report.json"
        result["timing_status"] = json.loads(path.read_text())["status"] if path.exists() else "evaluation-error"
        if result["timing_status"] != "extracted":
            return result | {"status": "timing-unavailable"}
        if source["status"] != "ready":
            return result | {"status": "source-prerequisite-unqualified"}

        published = SourceProductions(Path(source["collection"]), Path(source["plan"]))
        primary = published.plan["targets"]["configurations"][case["configuration"]]
        prediction = self.root / "targets" / self.name / name / "prediction"
        command = self.commands.command(
            "predict", *published.arguments(primary), "--input", self.root / case["prediction_input"], "--output", prediction,
            "--binary", self.binary, "--modeling-image", self.images["modeling"], "--control-image", self.images["control"],
            "--timeout", self.budget["task_seconds"], "--execution-trace", "--execution-inputs", output / "timing/execution-inputs.json",
        )
        code = self.process(f"{name}/predict", command)
        result["prediction_exit"] = code
        execution = prediction / ("execution.jsonl" if code == 0 else "execution.partial.jsonl")
        if not execution.exists():
            return result | {"status": "prediction-no-trace"}
        result["execution_trace"] = str(execution)
        result["comparison_exit"] = self.evaluate(f"{name}/compare", "compare_actions.py", "--capture", native,
                                                  "--graphs", production / "work", "--execution", execution,
                                                  "--output", output / "comparison")
        path = output / "comparison/actions.json"
        if not path.exists():
            return result | {"status": "comparison-error"}
        report = json.loads(path.read_text())
        return result | {"status": "compared", "comparison": report["status"], "coverage": report["coverage"],
                         "counts": report["counts"], "actions_compared": report["actions_compared"], "report": str(path)}

    def run(self, phase, domain_name):
        if self.matrix["status"] != "frozen":
            raise ValueError("matrix is not frozen; no execution started")
        sources_path, targets_path = self.results / "sources.json", self.results / "targets.json"
        sources = json.loads(sources_path.read_text()) if sources_path.exists() else {}
        targets = json.loads(targets_path.read_text()) if targets_path.exists() else {}
        comparisons_path = self.results / "comparisons.json"
        comparisons = json.loads(comparisons_path.read_text()) if comparisons_path.exists() else {}
        domains = [row for row in self.matrix["domains"] if domain_name is None or row["id"] == domain_name]
        if not domains:
            raise ValueError("matrix contains no selected domain")

        records, output = {
            "sources": (sources, sources_path), "targets": (targets, targets_path),
            "compare": (comparisons, comparisons_path),
        }[phase]
        with ExitStack() as contexts:
            lock = contexts.enter_context((self.steps / f".{phase}.lock").open("a"))
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            if phase != "compare":

                # Native stages share the physical cards; the independent CPU comparison lane can continue.
                native = contexts.enter_context((self.logs / ".native.lock").open("a"))
                fcntl.flock(native, fcntl.LOCK_EX | fcntl.LOCK_NB)
            for domain in domains:
                for case in [domain] if phase == "sources" else domain["targets"]:
                    try:
                        if phase == "sources":
                            result = self.sources(domain)
                        elif phase == "targets":
                            result = self.target(domain, case)
                        else:
                            source = sources.get(domain["id"], {"status": "not-executed"})
                            target = targets.get(case["id"], {"status": "not-executed"})
                            result = self.compare(domain, case, source, target)
                    except (OSError, ValueError, KeyError, IndexError, TypeError) as error:

                        # Bad artifacts have a terminal finding; unresolved processes still stop above.
                        result = {"id": case["id"], "status": "artifact-error", "error": f"{type(error).__name__}: {error}"}
                    records[case["id"]] = result
                    write_json(output, records, indent=2)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True, help="existing investigation directory")
    parser.add_argument("--matrix", type=Path, required=True, help="frozen matrix beside environment.json and markov")
    parser.add_argument("--phase", choices=("sources", "targets", "compare"), required=True)
    parser.add_argument("--domain", help="one named domain; omit for the whole stage")
    arguments = parser.parse_args()
    MatrixRun(arguments.root, arguments.matrix).run(arguments.phase, arguments.domain)


if __name__ == "__main__":
    main()
