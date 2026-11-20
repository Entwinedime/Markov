"""Run bounded structure and performance comparisons on a frozen matrix."""

import argparse
import fcntl
import json
import re
import time
from contextlib import ExitStack
from pathlib import Path

from markov_adapters.artifacts import write_json
from markov_adapters.sglang.configuration import CaptureInput
from markov_workflow.capture import CaptureProcess
from markov_workflow.containers import OwnedContainers
from markov_workflow.limits import OperationLimit
from markov_workflow.source_productions import SourceProductions

from run import TaskCommands


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
        self.commands = TaskCommands(self.repository, self.images)
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

    @staticmethod
    def mapped(report):
        return (bool(report["ranks"]) and report["native_case"]["status"] in {"consistent", "incomplete"}
                and all(row["mapping"] == "consistent" and row["timing"]["status"] == "compatible" for row in report["ranks"]))

    def sources(self, domain):
        name = domain["id"]
        if "source" in domain:
            source = domain["source"]
            collection = Path(source["collection"]).resolve(strict=True)

            # Fixed-library validation checks its explicit original sources without planning or collecting from the new targets.
            return {"id": name, "collection": str(collection), "source_kind": "fixed_library"} | self.source_evidence(name, collection)
        plan = self.root / "assets" / self.name / name / "plan"
        code = self.process(f"{name}/plan", self.commands.command(
            "markov_workflow", "plan-sources", "--input", self.root / domain["exploration"], "--output", plan,
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
            "markov_workflow", "acquire-sources", "--plan", plan, "--deployment", self.root / domain["deployment"], "--output", collection,
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

        return result | self.source_evidence(name, collection)

    def source_evidence(self, name, collection):
        published = SourceProductions(collection)
        checks = []
        for job in published.plan["sources"]:
            identity = job["id"]
            observation = CaptureProcess.published(published.captures[identity], published.jobs[str(identity)])
            output = self.results / "sources" / name / str(identity)
            command = [
                "docker", "run", "--rm", "--init", "--runtime", "runc", "--network", "none",
                "--mount", f"type=bind,src={self.repository},dst={self.repository}",
                "--env", "PYTHONPATH=src/verification:src/collection:src/adapters:src/workload",
                "--workdir", str(self.repository), "--entrypoint", "timeout", self.images["control"],
                "--signal=KILL", str(self.budget["operation_seconds"]), "python3", "-m", "markov_verification.source_evidence",
                "--production", str(published.paths[identity]), "--capture", str(observation), "--output", str(output),
            ]
            self.process(f"{name}/source-evidence-{identity}", command)
            path = output / "source-evidence.json"
            report = json.loads(path.read_text()) if path.exists() else None
            checks.append({"job": identity, "report": str(path), "mapped": report is not None and self.mapped(report),
                           "native_group_status": None if report is None else report["native_case"]["status"]})
        return {"status": "ready" if all(row["mapped"] for row in checks) else "source-evidence-unqualified", "jobs": checks}

    def prepare_targets(self, domains, sources, targets):
        for domain in domains:
            source = sources.get(domain["id"], {"status": "not-executed"})
            published = SourceProductions(Path(source["collection"])) if source["status"] == "ready" else None
            for case in domain["targets"]:
                native = json.loads((self.root / case["capture_input"]).read_text())
                prediction = json.loads((self.root / case["prediction_input"]).read_text())
                serving = CaptureInput(native)
                changed = [name for name in ("model", "instances", "workload") if native[name] != prediction[name]]
                if changed:
                    raise ValueError(f"{case['id']}: native and prediction inputs differ in {changed}")
                requests = [request["id"] for request in serving.workload["requests"]]
                if serving.submission_groups != [requests]:
                    raise ValueError(f"{case['id']}: a target requires one timed cohort in declared request order")

                # A missing source is a separate finding; available libraries must use the real prediction assembly.
                if published is not None:
                    try:
                        published.prepare(prediction)
                    except ValueError as error:
                        raise ValueError(f"{case['id']}: {error}") from error
                target = targets.get(case["id"], {})
                if "observation" in case or "native" in target:
                    owner = self.root / case.get("observation", f"targets/{self.name}/{case['id']}/native")
                    observation = CaptureProcess.published(owner, native)
                    if "native" in target and observation != Path(target["native"]).resolve(strict=True):
                        raise ValueError(f"{case['id']}: target report refers to a different native observation")

    def target(self, domain, case):
        name = case["id"]
        directory = self.root / "targets" / self.name / name
        task = self.root / case["capture_input"]
        result = {"id": name}
        if "observation" in case:

            # An explicit immutable task replaces collection; the same complete input check still precedes production.
            output = (self.root / case["observation"]).resolve(strict=True)
            result["observation"] = str(output)
        else:
            output = directory / "native"
            mode = case.get("detailed_capture", "resource-trace")
            if mode not in {"resource-trace", "service-reference"}:
                raise ValueError("a structure target needs a detailed native observation")
            timeout = case.get("detailed_timeout_seconds", self.budget["task_seconds"])
            code = self.observe(f"{name}/capture", domain, task, output, mode, timeout)
            result["capture_exit"] = code
            if code:
                return result | {"status": "native-capture-failed"}

        native = CaptureProcess.published(output, json.loads(task.read_text()))
        production = directory / "structure"
        code = self.process(f"{name}/produce", self.commands.command(
            "markov_workflow", "produce", "--capture", native, "--product", "graphs", "--output", production, "--binary", self.binary,
            "--source-image", self.images["source"], "--modeling-image", self.images["modeling"],
            "--budget-seconds", self.budget["operation_seconds"], "--keep-intermediates",
        ))
        result.update(native=str(native), production=str(production), production_exit=code)
        return result | {"status": "target-production-failed" if code else "produced"}

    def observe(self, step, domain, task, output, mode, timeout):
        command = self.commands.command(
            "markov_workflow", "capture", "--input", task, "--deployment", self.root / domain["deployment"], "--output", output,
            "--source-image", self.images["source"], "--timeout", timeout, "--budget-seconds", timeout,
            "--attempt-limit", 1, f"--{mode}", model=json.loads(task.read_text())["model"]["path"],
        )
        return self.step(step, command, {"native_seconds": timeout + 60, "native_attempts": 1},
                         timeout + 60, output / "execution.json")

    def compare(self, case, source, target):
        name = case["id"]
        result = {"id": name, "source_status": source["status"], "target_status": target["status"]}
        if target["status"] != "produced":
            return result | {"status": "target-prerequisite-unqualified"}
        if source["status"] != "ready":
            return result | {"status": "source-prerequisite-unqualified"}

        output = self.results / "cases" / name
        command = self.commands.command(
            "markov_verification.structure", "--capture", target["native"], "--reference", target["production"],
            "--production", source["collection"], "--input", self.root / case["prediction_input"], "--output", output,
            "--binary", self.binary, "--modeling-image", self.images["modeling"], "--control-image", self.images["control"],
            "--timeout", self.budget["task_seconds"],
        )

        # One process holds the checked native case through extraction, prediction and comparison.
        allowance = 3 * self.budget["operation_seconds"] + 2 * self.budget["task_seconds"] + 60
        result["verification_exit"] = self.step(f"{name}/verify", command, {"processing_seconds": allowance}, allowance)
        path = output / "result.json"
        if not path.exists():
            return result | {"status": "verification-error"}
        return result | json.loads(path.read_text()) | {"report": str(path)}

    def performance(self, phase, domain, case, source, target):
        name = case["id"]
        result = {"id": name}
        if phase == "fixed" and case.get("detailed_capture") != "service-reference":
            return result | {"status": "not-selected"}
        if "costs" not in domain:
            return result | {"status": "cost-asset-unavailable"}
        if phase == "fixed":
            if target["status"] != "produced":
                return result | {"status": "target-prerequisite-unqualified"}
            module = "fixed_graph"
            evidence = ("--capture", target["native"], "--reference", target["production"])
        else:
            module = "autonomous"
            latency = self.root / "targets" / self.name / name / "latency"
            timeout = case.get("latency_timeout_seconds", self.budget["task_seconds"])
            code = self.observe(f"{name}/latency", domain, self.root / case["capture_input"], latency, "latency-only", timeout)
            if code:
                return result | {"status": "native-capture-failed", "capture_exit": code}

            # Keep the frozen native observation even when the current prediction asset is unusable.
            result["native_task"] = str(latency)
            if source["status"] != "ready":
                return result | {"status": "source-prerequisite-unqualified"}
            evidence = ("--capture", latency, "--production", source["collection"],
                        "--input", self.root / case["prediction_input"], "--control-image", self.images["control"])

        output = self.results / phase / name
        timeout = self.budget["operation_seconds"]
        command = self.commands.command(
            f"markov_verification.{module}", *evidence, "--costs", self.root / domain["costs"], "--output", output,
            "--binary", self.binary, "--modeling-image", self.images["modeling"], "--timeout", timeout,
        )
        allowance = (2 * timeout if phase == "fixed" else timeout) + 60
        code = self.step(f"{name}/{phase}", command, {"processing_seconds": allowance}, allowance)
        path = output / "result.json"
        if not path.exists():
            return result | {"status": "verification-error", "verification_exit": code}
        return result | {"status": json.loads(path.read_text())["status"], "report": str(path)}

    def run(self, phase, domain_name):
        if self.matrix["status"] != "frozen":
            raise ValueError("matrix is not frozen; no execution started")
        domains = [row for row in self.matrix["domains"] if domain_name is None or row["id"] == domain_name]
        if not domains:
            raise ValueError("matrix contains no selected domain")

        report = "comparisons" if phase == "compare" else phase
        output = self.results / f"{report}.json"
        with ExitStack() as contexts:
            lock = contexts.enter_context((self.steps / f".{phase}.lock").open("a"))
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            results = {}
            for name in {"sources", "targets", report}:
                path = self.results / f"{name}.json"
                results[name] = json.loads(path.read_text()) if path.exists() else {}
            sources, targets, records = (results[name] for name in ("sources", "targets", report))
            if phase != "sources":

                # Check the entire selected range before spending any native allowance or comparing unlike inputs.
                self.prepare_targets(domains, sources, targets)
            needs_native = (
                phase == "autonomous"
                or phase == "sources" and any("source" not in domain for domain in domains)
                or phase == "targets" and any("observation" not in case for domain in domains for case in domain["targets"])
            )
            if needs_native:

                # Only actual collection occupies the cards; explicit observation reuse stays in the CPU lane.
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
                            if phase == "compare":
                                result = self.compare(case, source, target)
                            else:
                                result = self.performance(phase, domain, case, source, target)
                    except (OSError, ValueError, KeyError, IndexError, TypeError) as error:

                        # Bad artifacts have a terminal finding; unresolved processes still stop above.
                        result = {"id": case["id"], "status": "artifact-error", "error": f"{type(error).__name__}: {error}"}
                    records[case["id"]] = result
                    write_json(output, records, indent=2)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True, help="existing investigation directory")
    parser.add_argument("--matrix", type=Path, required=True, help="frozen matrix beside environment.json and markov")
    parser.add_argument("--phase", choices=("sources", "targets", "compare", "fixed", "autonomous"), required=True)
    parser.add_argument("--domain", help="one named domain; omit for the whole stage")
    arguments = parser.parse_args()
    MatrixRun(arguments.root, arguments.matrix).run(arguments.phase, arguments.domain)


if __name__ == "__main__":
    main()
