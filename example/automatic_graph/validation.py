#!/usr/bin/env python3
"""Compare a declared target with native execution while keeping its source assets fixed."""

from __future__ import annotations

import argparse
import fcntl
import json
import shutil
from pathlib import Path
from typing import Any

from markov_collection.artifacts.json_file import write_json
from markov_collection.workflow.exploration import BudgetExceeded, Exploration, read_specification
from serving import native_configuration


def analyze(task: Exploration, script: str, arguments: list[str]) -> None:
    command = ["python3", str(task.root / "example/automatic_graph" / script), *arguments]
    task.step(
        [
            "docker",
            "run",
            "--rm",
            "--init",
            "--network",
            "none",
            "--label",
            task.environment.label,
            "-v",
            f"{task.root}:{task.root}",
            "-e",
            f"PYTHONPATH={task.root}/src/collection",
            task.environment.modeling_image,
            *task.environment.limited(command, task.available("processing")),
        ],
        "processing",
    )


def completion_checks(target: dict[str, Any], result: dict[str, Any]) -> dict[str, bool]:
    if result["control_status"] != "complete":
        return {"control": False}
    settings = target["configuration"]
    recovered = (
        result["pool_free_pages"]
        == {role: pool["max_total_tokens"] // settings["page_size"] for role, pool in settings["pd"].items()}
        if "pd" in settings
        else result["free_pages"] == settings["max_total_tokens"] // settings["page_size"]
    )
    expected = {request["id"]: request["output_tokens"] for request in target["requests"]}
    actual = {request["id"]: request["generated_tokens"] for request in result["requests"]}
    inputs = {request["id"]: request for request in target["requests"]}
    return {
        "control": True,
        "requests": len(result["requests"]) == len(expected) and expected == actual,
        "request_inputs": set(actual) == set(inputs)
        and all(
            request[field] == inputs[request["id"]][field]
            for request in result["requests"]
            for field in ("arrival_ns", "prompt_tokens", "output_tokens")
        ),
        "pool_recovery": recovered,
        "output_times": all(
            request["arrival_ns"]
            <= request["first_output_ns"]
            <= request["last_output_ns"]
            <= request["state_committed_ns"]
            for request in result["requests"]
        ),
    }


class NativeValidation:
    def __init__(
        self, task: Exploration, configuration: str, workload: str, capture: Path, minimum_members: int = 1
    ) -> None:
        self.task = task
        self.key = f"{configuration}/{workload}"
        domain = task.configurations[configuration]
        if workload not in {item["id"] for item in task.specification["workloads"]}:
            raise ValueError(f"unknown workload: {workload}")
        self.domain = task.domain_settings[domain]
        self.asset = task.directory / "assets" / domain / "templates.json"
        self.input = task.directory / "inputs/targets" / (self.key + ".json")
        self.capture = capture.resolve()
        self.capture.relative_to(task.directory / "captures")
        self.initial_capture = self.capture
        self.work = task.directory / "work/validation" / self.capture.name
        self.report = task.directory / "results/validation" / self.key / (self.capture.name + ".json")
        self.resuming = False
        self.minimum_members = minimum_members
        self.outcome: dict[str, Any] = {}

    def simulate(self, details: bool) -> Path:
        output = self.work / ("details.json" if details else "default.json")
        if self.resuming and output.is_file():
            return output
        arguments = ["simulate", "--templates", str(self.asset), "--input", str(self.input), "--output", str(output)]
        if details:
            arguments.append("--details")
        self.task.step(
            self.task.environment.tool_command(
                arguments, [self.asset, self.input], output, self.task.available("processing")
            ),
            "processing",
        )
        return output

    def observe(self, target: dict[str, Any]) -> None:
        if (
            self.resuming
            and self.capture.exists()
            and not self.outcome.get("native_complete", False)
            and not self.interpreted()
        ):
            self.capture = self.initial_capture.with_name(
                f"{self.initial_capture.name}-attempt-{self.task.state['native_attempts'] + 1}"
            )
        self.outcome["capture"] = str(self.capture.relative_to(self.task.directory))
        settings = target["configuration"]
        configuration = (
            {
                role: native_configuration({"page_size": settings["page_size"], **pool})
                for role, pool in settings["pd"].items()
            }
            if "pd" in settings
            else native_configuration(settings)
        )
        pairs = [[item["prompt_tokens"], item["output_tokens"]] for item in target["requests"]]
        if self.capture.exists():
            observed = json.loads((self.capture / "capture.json").read_text())
            expected = {**self.domain, "model": self.task.specification["model"]["path"], "requests": pairs}
            if "pd" in expected:
                expected["pd"] = [list(role) for role in expected["pd"]]
            if any(observed[key] != value for key, value in expected.items()):
                raise ValueError("native capture model, topology or requests differ from the case")
            pools = configuration.items() if "pd" in settings else (("model", configuration),)
            for role, pool in pools:
                actual = observed["configuration"][role] if "pd" in settings else observed["configuration"]
                if any(actual[key] != value for key, value in pool.items()):
                    raise ValueError("native capture pool configuration differs from the case")
            if "ep_routes" in target and (
                observed["serving_input"] is None or observed["serving_input"]["ep_routes"] != target["ep_routes"]
            ):
                raise ValueError("native capture routing differs from the case")
            if self.resuming and not self.interpreted():
                self.task.prepare()
                self.interpret()
            self.outcome["native_complete"] = True
            return
        task = self.task
        environment = task.environment
        if environment.native is None:
            task.prepare()
        self.capture.mkdir(parents=True)
        request = task.acquisition.request(
            task.specification,
            self.domain,
            0,
            environment.native_path(self.capture),
            environment.native_path(environment.observer_build / task.acquisition.selector_binary),
            task.model_configuration,
        )
        request["configuration"] = configuration
        request["requests"] = pairs
        request["serving_input"] = target if "ep_routes" in target else None
        request["observe_all_workers"] = True
        path = self.capture / "capture.json"
        write_json(path, request, indent=2)
        self.task.step(
            environment.native_command(
                task.acquisition.command("capture", environment.native_path(path)),
                self.capture,
                task.acquisition.observer_library,
                task.available("native"),
            ),
            "native",
        )
        self.outcome["native_complete"] = True
        self.interpret()

    def ranks(self) -> range:
        count = (
            sum(tp * pp for _, tp, pp, _ in self.domain["pd"])
            if "pd" in self.domain
            else self.domain["tp"] * self.domain["pp"]
        )
        return range(count)

    def interpreted(self) -> bool:
        return all(
            (self.capture / self.task.acquisition.source_name(self.domain, rank))
            .with_name(f"physical-rank-{rank}.json")
            .is_file()
            for rank in self.ranks()
        )

    def interpret(self) -> None:
        task = self.task
        environment = task.environment
        path = self.capture / "capture.json"
        task.step(
            environment.native_command(
                task.acquisition.command("interpret", environment.native_path(path)),
                self.capture,
                task.acquisition.observer_library,
                task.available("processing"),
            ),
            "processing",
        )

    def compare(self, rank: int, prediction: Path) -> dict[str, Any]:
        task = self.task
        source = self.capture / task.acquisition.source_name(self.domain, rank)

        # Target observation uses real process groups and exports every rank separately, including TP1.
        physical = source.with_name(f"physical-rank-{rank}.json")
        observed = self.work / f"rank-{rank}-observed.json"
        output = self.report.parent / self.capture.name / f"rank-{rank}.json"
        if self.resuming and output.is_file():
            report = json.loads(output.read_text())
            return {"rank": rank, "status": report["status"], "report": str(output.relative_to(task.directory))}
        arguments = ["produce", "--source", str(source), "--physical", str(physical), "--output", str(observed)]
        task.step(
            task.environment.tool_command(arguments, [source, physical], observed, task.available("processing")),
            "processing",
        )
        arguments = [
            "--source",
            str(source),
            "--physical",
            str(physical),
            "--observed",
            str(observed),
            "--prediction",
            str(prediction),
            "--input",
            str(self.input),
            "--output",
            str(output),
        ]
        analyze(task, "structure.py", arguments)
        report = json.loads(output.read_text())
        observed.unlink()
        return {"rank": rank, "status": report["status"], "report": str(output.relative_to(task.directory))}

    def evidence(self, name: str, arguments: list[str]) -> dict[str, Any]:
        output = self.report.parent / self.initial_capture.name / (name + ".json")
        if not self.resuming or not output.is_file():
            analyze(self.task, "evidence.py", ["--input", str(self.input), *arguments, "--output", str(output)])
        return json.loads(output.read_text())

    def run(self, resume: bool = False) -> dict[str, Any]:
        if self.report.exists():
            previous = json.loads(self.report.read_text())
            if not resume or previous["status"] not in ("interrupted", "budget_exhausted"):
                raise FileExistsError(f"validation report already exists: {self.report}")
            history = previous.pop("history", [])
            self.outcome = {
                key: value for key, value in previous.items() if key not in ("status", "error", "error_type")
            }
            self.outcome["history"] = [*history, previous]
            self.outcome["ranks"] = list(previous["ranks"])
            self.capture = self.task.directory / previous["capture"]
            self.resuming = True
        else:
            self.outcome = {
                "case": self.key,
                "capture": str(self.capture.relative_to(self.task.directory)),
                "ranks": [],
            }
        outcome = self.outcome
        try:
            target = json.loads(self.input.read_text())
            if any(request["arrival_ns"] != 0 for request in target["requests"]):
                raise ValueError("native comparison requires simultaneous arrivals")
            prediction = None
            try:
                plain = json.loads(self.simulate(False).read_text())
                prediction = self.simulate(True)
                detailed = json.loads(prediction.read_text())
                diagnostic = {"graph", "decisions", "outputs", "controls", "execution_timeline"}
                outcome["same_default_details"] = plain == {
                    key: value for key, value in detailed.items() if key not in diagnostic
                }
                outcome["completion"] = completion_checks(target, plain)
                del detailed
                outcome["execution_evidence"] = self.evidence("execution", ["--prediction", str(prediction)])
            except BudgetExceeded:
                raise
            except RuntimeError as error:
                outcome["prediction_error"] = str(error)

            # Preselected native cases still run when prediction fails; observations never build templates.
            self.observe(target)
            try:
                outcome["native_evidence"] = self.evidence(
                    "native",
                    [
                        "--minimum-batch-members",
                        str(self.minimum_members),
                        "--source",
                        *(
                            str(self.capture / self.task.acquisition.source_name(self.domain, rank))
                            for rank in self.ranks()
                        ),
                    ],
                )
            except BudgetExceeded:
                raise
            except (OSError, RuntimeError) as error:
                outcome["native_evidence"] = {
                    "status": "error",
                    "error_type": type(error).__name__,
                    "error": str(error),
                }
            if prediction is not None:
                completed = {item["rank"] for item in outcome["ranks"]}
                for rank in self.ranks():
                    if rank in completed:
                        continue
                    try:
                        outcome["ranks"].append(self.compare(rank, prediction))
                    except BudgetExceeded:
                        raise
                    except (OSError, RuntimeError) as error:
                        outcome["ranks"].append(
                            {"rank": rank, "status": "error", "error_type": type(error).__name__, "error": str(error)}
                        )
            statuses = {item["status"] for item in outcome["ranks"]}
            statuses.update(
                outcome[key]["status"] for key in ("native_evidence", "execution_evidence") if key in outcome
            )
            if "prediction_error" in outcome or "error" in statuses:
                outcome["status"] = "error"
            elif (
                not all(outcome["completion"].values())
                or outcome.get("same_default_details") is False
                or "differences" in statuses
            ):
                outcome["status"] = "differences"
            else:
                outcome["status"] = "matched" if statuses == {"matched"} else "partial"
        except BaseException as error:
            status = "error"
            if isinstance(error, KeyboardInterrupt):
                status = "interrupted"
            elif isinstance(error, BudgetExceeded):
                status = "budget_exhausted"
            outcome.update(
                status=status,
                error_type=type(error).__name__,
                error=str(error),
            )
            self.task.environment.cleanup()
            if isinstance(error, (KeyboardInterrupt, BudgetExceeded)):
                raise
        finally:
            write_json(self.report, outcome, indent=2)
            if self.work.exists() and outcome["status"] not in ("interrupted", "budget_exhausted"):
                shutil.rmtree(self.work)
        return outcome


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", type=Path, required=True)
    parser.add_argument("--configuration", required=True)
    parser.add_argument("--workload", required=True)
    parser.add_argument("--capture", type=Path, required=True)
    arguments = parser.parse_args()
    directory = arguments.run.resolve()
    directory.relative_to(Path.cwd() / "data")
    with (directory / ".lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        task = Exploration(Path.cwd(), directory, read_specification(directory / "inputs/task.json"), False)
        try:
            report = NativeValidation(
                task, arguments.configuration, arguments.workload, directory / arguments.capture
            ).run()
            print(json.dumps(report))
            if report["status"] in ("error", "interrupted"):
                raise SystemExit(2)
        finally:
            task.environment.cleanup()
            task.report()
