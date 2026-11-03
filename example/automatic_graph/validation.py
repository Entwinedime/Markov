#!/usr/bin/env python3
"""Compare a declared target with native execution while keeping its source assets fixed."""

from __future__ import annotations

import argparse
import fcntl
import json
import shutil
from pathlib import Path
from typing import Any

from markov_application.environment import NativeDeviceUnavailable
from markov_application.exploration import BudgetExceeded, Exploration
from markov_application.specification import read_specification, serving_input
from markov_collection.artifacts import write_json
from request_state import ResourceStateEvidence
from serving import native_configuration


def analyze(task: Exploration, script: str, arguments: list[str]) -> None:
    task.step(
        task.environment.python_command(
            [str(task.root / "example/automatic_graph" / script), *arguments], task.available("processing")
        ),
        "processing",
    )


def completion_checks(target: dict[str, Any], result: dict[str, Any]) -> dict[str, bool]:
    if result["control_status"] != "complete":
        return {"control": False}
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
        "pool_recovery": all(ResourceStateEvidence(result["resources"]).summary_checks().values()),
        "output_times": all(
            request["arrival_ns"]
            <= request["first_output_ns"]
            <= request["last_output_ns"]
            <= request["state_committed_ns"]
            for request in result["requests"]
        ),
    }


class NativeValidation:
    resumable = {"interrupted", "budget_exhausted", "resource_blocked", "captured"}

    def __init__(
        self,
        task: Exploration,
        configuration: str,
        workload: str,
        capture: Path,
        minimum_members: int = 1,
        control: str | None = None,
    ) -> None:
        published_control = task.specification.get("control", "lightweight")
        self.control = control or published_control

        # Native comparison uses this case's observed times, never a prediction made with assumed durations.
        self.reuse_published = self.control == published_control and self.control != "native"

        # The task remains the sole owner of preparation, metadata, budgets and containers.
        self.task = task
        self.key = f"{configuration}/{workload}"
        domain = task.configurations[configuration]
        if workload not in {item["id"] for item in task.specification["workloads"]}:
            raise ValueError(f"unknown workload: {workload}")
        self.domain = task.domain_settings[domain]
        self.asset = task.asset_path(domain)
        self.input = task.directory / "inputs/targets" / (self.key + ".json")
        self.capture = capture.resolve()
        self.capture.relative_to(task.directory / "captures")
        self.initial_capture = self.capture
        self.report = task.directory / "results/validation" / self.key / self.control / (self.capture.name + ".json")
        self.resuming = False
        self.minimum_members = minimum_members
        self.outcome: dict[str, Any] = {}

    def simulate(self, details: bool) -> Path:
        published = self.task.state["results"].get(self.key)
        if self.reuse_published and details == self.task.details and published is not None:
            result = self.task.directory / published
            if result.is_file():
                return result

        basis = "observed-" if self.control == "native" else ""
        output = self.work / (basis + ("details.json" if details else "default.json"))
        if self.resuming and output.is_file():
            return output
        self.task.simulate(
            self.asset, self.input, output, details, control=self.control,
            completion_times=self.completion_times if self.control == "native" else None,
        )
        return output

    @staticmethod
    def summary(detailed: dict[str, Any]) -> dict[str, Any]:
        diagnostic = {"graph", "decisions", "outputs", "execution_timeline", "resource_history", "observed_work"}
        result = {key: value for key, value in detailed.items() if key not in diagnostic}
        result["resources"] = {
            pool: {key: value for key, value in resources.items() if key != "ownership"}
            for pool, resources in detailed["resources"].items()
        }
        return result

    @property
    def work(self) -> Path:

        # A resumed capture attempt owns its own predictions and timing interpretation.
        return self.task.directory / "work/validation" / self.capture.name / self.control

    @property
    def completion_times(self) -> Path:
        return self.observations / "completion-times.json"

    def prepare_feedback(self) -> None:
        if self.control != "native" or (self.resuming and self.completion_times.is_file()):
            return
        self.task.step(
            self.task.environment.python_command(
                [
                    "-m", "markov_collection.adapters.frameworks.sglang.completion_times",
                    "--capture", str(self.capture), "--output", str(self.completion_times),
                ],
                self.task.available("processing"),
            ),
            "processing",
        )

    def observe(self, target: dict[str, Any]) -> None:
        if (
            self.resuming
            and self.capture.exists()
            and not self.captured()
        ):
            self.capture = self.initial_capture.with_name(
                f"{self.initial_capture.name}-attempt-{self.task.state['native_attempts'] + 1}"
            )

            # Earlier evidence stays in history; the new native attempt starts its own derived results.
            history = self.outcome["history"]
            self.outcome.clear()
            self.outcome.update(case=self.key, control=self.control, ranks=[], history=history)
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
        if self.captured():
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
            for directory in self.task.acquisition.observation_directories(self.domain, self.capture):
                content = json.loads((directory / "session.json").read_text())["content"]
                if content != {"requests": target["requests"], "prefixes": target.get("prefixes", [])}:
                    raise ValueError("native capture content differs from the case")
            self.outcome["native_complete"] = True
            return
        task = self.task
        environment = task.environment

        # Isolated source roles may use one card; a real target needs every native participant.
        if len(environment.devices) < len(self.ranks()):
            raise NativeDeviceUnavailable(
                f"native target requires {len(self.ranks())} devices; "
                f"the source task declares {len(environment.devices)}"
            )
        if environment.native is None:
            invalid = task.prepare_native()
            if self.key in invalid:
                raise ValueError(invalid[self.key])

        # An explicit resume may reuse a request directory left by an unstarted device preflight.
        self.capture.mkdir(parents=True, exist_ok=self.resuming)
        request = task.capture_request(self.domain, 0, self.capture)
        request["configuration"] = configuration
        request["requests"] = pairs

        # Target observation replaces the discovery recipe, including its cache-priming submission groups.
        request["submissions"] = [list(range(len(pairs)))]
        request["serving_input"] = target
        request["observe_all_workers"] = True
        self.outcome["native_image"] = request["native_image"]
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

    def ranks(self) -> range:
        count = (
            sum(tp * pp for _, tp, pp, _ in self.domain["pd"])
            if "pd" in self.domain
            else self.domain["tp"] * self.domain["pp"]
        )
        return range(count)

    def captured(self) -> bool:

        # Native session publication, rather than an interpretation file, authorizes reuse without another Engine run.
        return (self.capture / "session-summary.json").is_file()

    def interpreted(self) -> bool:
        return all(
            (self.interpretation / self.task.acquisition.source_name(self.domain, rank))
            .with_name(f"physical-rank-{rank}.json")
            .is_file()
            for rank in self.ranks()
        )

    def interpret(self) -> None:
        task = self.task
        environment = task.environment
        if environment.native is None:
            environment.start()
        self.interpretation.mkdir(parents=True, exist_ok=True)
        task.step(
            environment.native_command(
                task.acquisition.command(
                    "interpret", environment.native_path(self.capture), environment.native_path(self.interpretation),
                ),
                self.interpretation,
                None,
                task.available("processing"),
            ),
            "processing",
        )

    @property
    def observations(self) -> Path:
        return self.task.directory / "work/validation" / self.capture.name / "observed"

    @property
    def interpretation(self) -> Path:
        return self.observations / "source"

    def collect_rank_reports(self) -> None:
        reports = []
        for rank in self.ranks():
            output = self.report.parent / self.capture.name / f"rank-{rank}.json"
            if output.is_file():
                report = json.loads(output.read_text())
                reports.append({
                    "rank": rank,
                    "status": report["status"],
                    "report": str(output.relative_to(self.task.directory)),
                })
        self.outcome["ranks"] = reports

    def prepare_rank(self, rank: int) -> dict[str, Any]:
        task = self.task
        source = self.interpretation / task.acquisition.source_name(self.domain, rank)
        physical = source.with_name(f"physical-rank-{rank}.json")
        observed = self.observations / f"rank-{rank}.json"
        output = self.report.parent / self.capture.name / f"rank-{rank}.json"
        case = {
            "rank": rank, "source": str(source), "physical": str(physical),
            "observed": str(observed), "output": str(output),
        }
        if output.is_file():
            return case

        # One atomically published observation belongs to the target, not to either control backend.
        if not observed.is_file():
            arguments = ["produce", "--source", str(source), "--physical", str(physical), "--output", str(observed)]
            try:
                task.step(
                    task.environment.tool_command(
                        arguments, [source, physical], observed, task.available("processing")
                    ),
                    "processing",
                )
            except BudgetExceeded:
                raise
            except (OSError, RuntimeError) as error:
                write_json(output, {
                    "rank": rank,
                    "status": "error",
                    "error_type": type(error).__name__,
                    "error": str(error),
                }, indent=2)
                return case
        return case

    def compare(self, prediction: Path) -> None:
        output = self.report.parent / self.capture.name / "communication.json"
        try:
            if not output.is_file():
                cases = [self.prepare_rank(rank) for rank in self.ranks()]
                request = self.work / "comparison.json"
                write_json(request, {
                    "prediction": str(prediction), "input": str(self.input),
                    "ranks": cases, "communication_output": str(output),
                }, indent=2)
                analyze(self.task, "structure.py", ["--input", str(request)])
        finally:

            # A timeout can interrupt later ranks after earlier reports have already been published.
            self.collect_rank_reports()
            if output.is_file():
                self.outcome["communication_evidence"] = json.loads(output.read_text())

    def release_observations(self, controls: set[str]) -> None:
        for control in controls:
            report = self.report.parent.parent / control / self.report.name
            if not report.is_file() or json.loads(report.read_text())["status"] != "matched":
                return
        if self.observations.is_dir():
            shutil.rmtree(self.observations)

    def evidence(self, name: str, arguments: list[str]) -> dict[str, Any]:
        shared = name == "native"

        # Native facts belong to this capture; the required member count is part of that check's input.
        output = (
            self.observations / f"native-evidence-{self.minimum_members}.json"
            if shared else self.report.parent / self.capture.name / (name + ".json")
        )
        if not output.is_file() or (not shared and not self.resuming):
            analyze(self.task, "evidence.py", ["--input", str(self.input), *arguments, "--output", str(output)])
        return json.loads(output.read_text())

    def run(self, resume: bool = False, *, capture_only: bool = False) -> dict[str, Any]:
        if self.report.exists():
            previous = json.loads(self.report.read_text())
            if not resume or previous["status"] not in self.resumable:
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
                "control": self.control,
                "capture": str(self.capture.relative_to(self.task.directory)),
                "ranks": [],
            }
        outcome = self.outcome
        try:
            configuration, workload = self.key.split("/")
            target = serving_input(
                next(item for item in self.task.specification["configurations"] if item["id"] == configuration),
                next(item for item in self.task.specification["workloads"] if item["id"] == workload),
                self.task.specification["node_durations_ns"],
            )
            if self.input.is_file() and json.loads(self.input.read_text()) != target:
                raise ValueError("published target differs from the declared configuration/workload")
            if not self.input.is_file():
                write_json(self.input, target)
            if not capture_only and not self.asset.is_file():
                raise FileNotFoundError(f"source library was not published: {self.asset}")

            # Fix native facts first; only their boundary times reach prediction, never their decisions or graph.
            self.observe(target)
            if capture_only:
                if self.task.storage_bytes(retained=True) > self.task.specification["budget"]["retained_bytes"]:
                    raise BudgetExceeded("retained storage budget exceeded after native capture")
                outcome["status"] = "captured"
                return outcome
            if not self.interpreted():
                self.interpret()
            try:
                outcome["native_evidence"] = self.evidence(
                    "native",
                    [
                        "--minimum-batch-members",
                        str(self.minimum_members),
                        "--delivery",
                        str(self.capture / "request-delivery.json"),
                        "--source",
                        *(
                            str(self.interpretation / self.task.acquisition.source_name(self.domain, rank))
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
            prediction = None
            try:
                self.prepare_feedback()
                plain = json.loads(self.simulate(False).read_text())
                prediction = self.simulate(True)
                detailed = json.loads(prediction.read_text())
                outcome["same_default_details"] = plain == self.summary(detailed)
                outcome["completion"] = completion_checks(target, plain)
                del detailed
                outcome["execution_evidence"] = self.evidence("execution", ["--prediction", str(prediction)])
                outcome.pop("prediction_error", None)
            except BudgetExceeded:
                raise
            except (OSError, RuntimeError, ValueError) as error:
                outcome["prediction_error"] = str(error)
            if prediction is not None:
                self.compare(prediction)
            statuses = {item["status"] for item in outcome["ranks"]}
            statuses.update(
                outcome[key]["status"]
                for key in ("native_evidence", "execution_evidence", "communication_evidence") if key in outcome
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
            if self.task.storage_bytes(retained=True) > self.task.specification["budget"]["retained_bytes"]:
                raise BudgetExceeded("retained storage budget exceeded after native validation")
        except BaseException as error:
            status = "error"
            if isinstance(error, KeyboardInterrupt):
                status = "interrupted"
            elif isinstance(error, BudgetExceeded):
                status = "budget_exhausted"
            elif isinstance(error, NativeDeviceUnavailable):
                status = "resource_blocked"
            outcome.update(
                status=status,
                error_type=type(error).__name__,
                error=str(error),
            )
            self.task.environment.cleanup()
            if isinstance(error, KeyboardInterrupt):
                raise
        finally:
            write_json(self.report, outcome, indent=2)
            if self.work.exists() and outcome["status"] == "matched":
                shutil.rmtree(self.work)
        return outcome


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", type=Path, required=True)
    parser.add_argument("--configuration", required=True)
    parser.add_argument("--workload", required=True)
    parser.add_argument("--capture", type=Path, required=True)
    parser.add_argument("--capture-only", action="store_true", help="save native facts without templates or prediction")
    arguments = parser.parse_args()
    directory = arguments.run.resolve()
    directory.relative_to(Path.cwd() / "data")
    with (directory / ".lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        task = Exploration(Path.cwd(), directory, read_specification(directory / "inputs/task.json"), False)
        try:
            validation = NativeValidation(
                task, arguments.configuration, arguments.workload, directory / arguments.capture
            )
            outcome = validation.run(resume=True, capture_only=arguments.capture_only)
            validation.release_observations({validation.control})
            print(json.dumps(outcome))
            if outcome["status"] not in {"matched", "partial", "differences", "captured"}:
                raise SystemExit(2)
        finally:
            task.environment.cleanup()
            task.report()
