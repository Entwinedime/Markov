"""Public collection, prediction and recovery commands."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

from markov_collection.adapters.runtimes.ascend.capture import AscendCaptureRuntime
from markov_collection.artifacts.json_file import read_json
from markov_collection.workflow.collection import CollectionWorkflow


class WorkflowCommandLine:
    def __init__(self) -> None:
        self.parser = argparse.ArgumentParser(description=__doc__)
        self.commands = self.parser.add_subparsers(dest="command", required=True)

        for name in ("predict", "explore", "profile", "sample", "observe-target"):
            command = self.commands.add_parser(name)
            command.add_argument("--project", type=Path, required=True)
            if name == "explore":
                command.add_argument("--model", type=self._model_path, help="override the model/tokenizer path")
                command.add_argument("--devices", type=self._devices, help="deployment device IDs, e.g. 4,5")
            if name in {"explore", "observe-target"}:
                command.add_argument("--budget-sec", type=self._positive, required=True)
            if name in {"profile", "sample", "observe-target"}:
                command.add_argument("--configuration", required=True)
            if name == "profile":
                workload = command.add_mutually_exclusive_group(required=True)
                workload.add_argument("--workload")
                workload.add_argument("--representative", action="store_true")
            if name == "observe-target":
                command.add_argument("--workload", required=True)
                command.add_argument("--purpose", choices=["ports", "structure"], default="ports")
            if name == "sample":
                command.add_argument("--workload")
                command.add_argument("--kind", choices=["transfer", "device", "communication"], default="transfer")
                command.add_argument("--measurement-requests", type=Path)
                command.add_argument("--repetitions", type=self._positive, default=3)

        for name in ("status", "resume", "compare"):
            command = self.commands.add_parser(name)
            command.add_argument("--run", type=Path, required=True)
            if name == "compare":
                command.add_argument("--validation-manifest", type=Path, action="append", required=True)
            if name == "resume":
                command.add_argument("--template-assets", type=Path)
                command.add_argument("--performance-manifest", type=Path, action="append")
                command.add_argument("--cost-model", type=Path, action="append")
                for role in ("base", "performance", "validation"):
                    command.add_argument(f"--{role}-budget-sec", type=self._positive, help="new cumulative total")

    @staticmethod
    def _positive(value: str) -> int:
        number = int(value)
        if number <= 0:
            raise argparse.ArgumentTypeError("value must be a positive integer")
        return number

    @staticmethod
    def _model_path(value: str) -> Path:
        path = Path(value)
        if not path.is_absolute():
            raise argparse.ArgumentTypeError("model path must be absolute and visible to the native container")
        return path

    @staticmethod
    def _devices(value: str) -> list[int]:
        try:
            devices = [int(item) for item in value.split(",")]
        except ValueError:
            raise argparse.ArgumentTypeError("devices require comma-separated integer IDs") from None
        if any(item < 0 for item in devices) or len(set(devices)) != len(devices):
            raise argparse.ArgumentTypeError("device IDs must be nonnegative and unique")
        return devices

    def read(self) -> argparse.Namespace:
        arguments = self.parser.parse_args()
        if arguments.command == "sample":
            command = self.commands.choices["sample"]
            communication = arguments.kind == "communication"
            if communication and (arguments.workload is not None or arguments.measurement_requests is not None):
                command.error(
                    "communication sampling uses declared device assets, without workload/measurement requests"
                )
            if not communication and (arguments.workload is None or arguments.measurement_requests is None):
                command.error("transfer/device sampling requires --workload and --measurement-requests")
        return arguments


def main() -> None:
    command_line = WorkflowCommandLine()
    arguments = command_line.read()
    parser = command_line.parser

    root = Path(__file__).resolve().parents[4]
    workflow = CollectionWorkflow(root, AscendCaptureRuntime())
    if arguments.command in {"predict", "explore"}:
        from markov_collection.workflow.prediction import PredictionWorkflow

        result = PredictionWorkflow(workflow).predict(
            arguments.project.resolve(),
            validation_budget_sec=arguments.budget_sec if arguments.command == "explore" else None,
            model_path=arguments.model if arguments.command == "explore" else None,
            devices=arguments.devices if arguments.command == "explore" else None,
        )
    elif arguments.command in {"profile", "sample", "observe-target"}:
        result = workflow.profile(
            arguments.project.resolve(),
            role={"profile": "base", "sample": "performance", "observe-target": "validation"}[arguments.command],
            configuration_id=arguments.configuration,
            workload_id=arguments.workload,
            budget_sec=arguments.budget_sec if arguments.command == "observe-target" else None,
            purpose=arguments.purpose if arguments.command == "observe-target" else None,
            representative=arguments.representative if arguments.command == "profile" else False,
            repetitions=arguments.repetitions if arguments.command == "sample" else 3,
            measurement_kind=arguments.kind if arguments.command == "sample" else "transfer",
            measurement_requests=arguments.measurement_requests.resolve()
            if arguments.command == "sample" and arguments.measurement_requests is not None
            else None,
        )
    else:
        directory = arguments.run.resolve()
        state = read_json(directory / "run.json")
        budgets = {}
        if arguments.command == "resume":
            budgets = {
                role: seconds
                for role, seconds in [
                    ("base", arguments.base_budget_sec),
                    ("performance", arguments.performance_budget_sec),
                    ("validation", arguments.validation_budget_sec),
                ]
                if seconds is not None
            }
        if "prediction" in state:
            if "validation" in budgets and state["validation"] is None:
                parser.error("validation budget requires an exploration or observe-target task")
            from markov_collection.workflow.prediction import PredictionWorkflow

            prediction = PredictionWorkflow(workflow)
            if arguments.command == "compare":
                result = prediction.compare(directory, [path.resolve() for path in arguments.validation_manifest])
            elif arguments.command == "status":
                result = prediction.status(directory)
            else:
                result = prediction.resume(
                    directory,
                    budgets=budgets,
                    template_assets=arguments.template_assets.resolve()
                    if arguments.template_assets is not None
                    else None,
                    performance_manifests=(
                        [path.resolve() for path in arguments.performance_manifest]
                        if arguments.performance_manifest is not None
                        else None
                    ),
                    cost_models=[path.resolve() for path in arguments.cost_model]
                    if arguments.cost_model is not None
                    else None,
                )
        else:
            if arguments.command == "compare":
                parser.error("comparison requires a prediction task")
            if arguments.command == "resume" and (
                arguments.template_assets is not None
                or arguments.performance_manifest is not None
                or arguments.cost_model is not None
            ):
                parser.error("asset revisions require a prediction task")
            result = (
                workflow.status(directory)
                if arguments.command == "status"
                else workflow.resume(directory, budgets=budgets)
            )

    display = result
    if "prediction" in result:
        # Reports summarize the run; full requests and machine handoffs have
        # separate durable owners. The CLI shows progress and their references.
        display = {}
        for field in (
            "run",
            "project",
            "task_status",
            "status",
            "reason",
            "live_attempt",
            "active_wall_time_ns",
            "prediction_live",
            "comparison_live",
            "performance_live",
            "workflow_busy",
            "requested_changes_applied",
            "requested_comparison_applied",
            "active_representative",
            "collection_performed",
            "collection",
            "collection_usage",
            "assumptions",
            "request_results",
            "prediction",
            "performance_plan",
            "capture",
            "comparison",
            "comparison_task",
            "summary",
            "validation",
        ):
            if field in result:
                display[field] = result[field]

        active = result["active_performance"]
        display["active_performance"] = (
            {
                field: result["performance_sampling"][active][field]
                for field in ("configuration_id", "measurement_kind", "status")
            }
            if active is not None
            else None
        )

        display["combinations"] = []
        for combination in result.get("combinations", []):
            summary = {}
            for field in (
                "configuration_id",
                "request_workload_id",
                "status",
                "scope",
                "timing_basis",
                "assumptions",
                "gaps",
            ):
                if field in combination:
                    summary[field] = combination[field]

            metrics = combination.get("metrics")
            summary["metrics"] = None
            summary["request_count"] = None
            if metrics is not None:
                summary["metrics"] = metrics
                summary["request_count"] = metrics["request_count"]
            display["combinations"].append(summary)

    exploration = result.get("validation") is not None
    if exploration and arguments.command != "status":
        print(f"Run: {result['run']}\nStatus: {result['status']}")
        for field, activity in (
            ("workflow_busy", "existing workflow is running"),
            ("live_attempt", "native collection is running"),
            ("prediction_live", "prediction is running"),
            ("comparison_live", "comparison is running"),
            ("performance_live", "cost fitting is running"),
        ):
            if result.get(field):
                print(f"Activity: {activity}")
                break
        if result.get("reason"):
            print(f"Reason: {result['reason']}")
        for name in ("summary", "comparison"):
            if result[name] is not None:
                print(f"{name.capitalize()}: {Path(result['run']) / result[name]}")
    else:
        print(json.dumps(display, indent=2))
    if result.get("status") == "failed" and arguments.command != "status":
        raise SystemExit(1)
    if arguments.command == "compare" and result.get("requested_comparison_applied") is False:
        raise SystemExit(2)
    if (
        (arguments.command == "compare" or exploration)
        and arguments.command != "status"
        and result["status"] != "complete"
    ):
        raise SystemExit(2)


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        print(error.stderr.strip() if isinstance(error.stderr, str) else str(error), file=sys.stderr)
        raise SystemExit(2) from None
    except (ValueError, OSError, KeyError, TypeError) as error:
        print(error, file=sys.stderr)
        raise SystemExit(2) from None
    except RuntimeError as error:
        print(error, file=sys.stderr)
        raise SystemExit(1) from None
