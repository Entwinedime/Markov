"""Parse public task commands and select their concrete application handlers."""

import argparse
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    planning = commands.add_parser("plan-sources", help="select and publish an initial capture input per source domain")
    planning.add_argument("--input", type=Path, required=True)
    planning.add_argument("--output", type=Path, required=True)
    prediction = commands.add_parser("predict", help="predict from published template productions")
    prediction.add_argument("--production", type=Path, required=True, help="one complete source job supplying target workers")
    prediction.add_argument("--local-production", type=Path, action="append", default=[], help="additional source of compatible local implementations")
    prediction.add_argument("--input", type=Path, required=True)
    prediction.add_argument("--timeout", type=float, required=True)
    prediction.add_argument("--control-image", default="markov-next-control:ubuntu22.04")
    prediction.add_argument("--execution-trace", action="store_true", help="stream optional expanded graphs, dependencies and completion events")
    prediction.add_argument("--execution-inputs", type=Path, help="supply checked per-actor time conditions and native ingress rounds")
    production = commands.add_parser("produce", help="project a closed native capture into graphs or source templates")
    production.add_argument("--product", choices=("graphs", "templates"), default="templates")
    production.add_argument("--capture", type=Path, required=True)
    production.add_argument("--budget-seconds", type=float, required=True, help="cumulative allowance across resumes; may be explicitly raised")
    production.add_argument("--source-image", default="markov-next-sglang:ubuntu22.04")
    production.add_argument("--keep-intermediates", action="store_true")
    capture = commands.add_parser("capture", help="capture or resume a native task with cumulative allowances")
    capture.add_argument("--input", type=Path, required=True)
    capture.add_argument("--deployment", type=Path, required=True)
    capture.add_argument("--output", type=Path, required=True)
    capture.add_argument("--timeout", type=float, required=True)
    capture.add_argument("--budget-seconds", type=float, required=True, help="cumulative native allowance across attempts and resumes")
    capture.add_argument("--attempt-limit", type=int, required=True, help="cumulative native attempts; each invocation starts at most one")
    capture.add_argument("--source-image", default="markov-next-sglang:ubuntu22.04")
    capture.add_argument("--resource-trace", action="store_true", help="record optional resource and input-read evidence")
    acquisition = commands.add_parser("acquire-sources", help="capture and compile a finite plan under shared cumulative allowances")
    acquisition.add_argument("--plan", type=Path, required=True)
    acquisition.add_argument("--observations", type=Path, help="JSON map of planned domain IDs to published capture task paths; native inputs must match")
    acquisition.add_argument("--deployment", type=Path, required=True)
    acquisition.add_argument("--timeout", type=float, required=True, help="maximum seconds per native attempt")
    acquisition.add_argument("--native-budget-seconds", type=float, required=True)
    acquisition.add_argument("--postprocessing-budget-seconds", type=float, required=True)
    acquisition.add_argument("--attempt-limit", type=int, required=True)
    acquisition.add_argument("--storage-bytes", type=int, required=True, help="monitored logical bytes under this collection root")
    acquisition.add_argument("--source-image", default="markov-next-sglang:ubuntu22.04")
    acquisition.add_argument("--keep-intermediates", action="store_true")
    acquisition.add_argument("--resource-trace", action="store_true", help="record optional resource and input-read evidence")
    for command in (production, prediction, acquisition):
        command.add_argument("--output", type=Path, required=True)
        command.add_argument("--binary", type=Path, required=True)
        command.add_argument("--modeling-image", default="markov-next-modeling:ubuntu24.04")
    arguments = parser.parse_args()
    if arguments.command == "plan-sources":
        from markov_workflow.planning import PlanningProcess

        PlanningProcess(arguments).run()
    elif arguments.command == "produce":
        from markov_workflow.production import CaptureProduction

        CaptureProduction(arguments).run()
    elif arguments.command == "capture":
        from markov_workflow.capture import CaptureProcess

        CaptureProcess(arguments).run()
    elif arguments.command == "acquire-sources":
        from markov_workflow.acquisition import SourceAcquisition

        SourceAcquisition(arguments).run()
    else:
        from markov_workflow.prediction import PredictionProcess

        PredictionProcess(arguments).run()


if __name__ == "__main__":
    main()
